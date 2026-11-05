#!/usr/bin/env python3
"""Inspect sampled screen-processing dependencies; requests are not GPU writes."""
import argparse
import json


def records(lines):
    events, passes = [], {}
    for line in lines:
        for tag in ("flow", "pass-detail"):
            marker = "QVFR " + tag + " "
            if marker not in line:
                continue
            record = json.loads(line.split(marker, 1)[1])
            if tag == "flow":
                events.append(record)
            else:
                passes[record["frame"], record["pass"]] = record["gpu_ms"]
    return events, passes


def image(view):
    return view.get("image", view) if view else None


def identity(view):
    value = image(view)
    return (value["cookie"], value["storage"]) if value else None


def describe(view):
    value = image(view)
    if not value:
        return "none"
    return f'{value["cookie"]}@{value["storage"]} {view.get("width", value["width"])}x{view.get("height", value["height"])} f{view.get("format", value["format"])} s{value["samples"]}'


def summarize(events, passes, frame):
    # Frame-local, nearest observed producer only. Partial writes, unlogged
    # scene draws and skipped events mean these are possible dependencies.
    producers, output, seen_passes = {}, [], set()
    for event in sorted((e for e in events if e["frame"] == frame), key=lambda e: e["seq"]):
        seq, kind = event["seq"], event["kind"]
        dest = event.get("output")
        if kind == "draw":
            key = frame, event.get("pass")
            if key not in seen_passes:
                duration = passes.get(key)
                output.append(f'pass {key[1]}: {duration if duration is not None else "unranked/unavailable"} ms (whole pass, counted once)')
                seen_passes.add(key)
            shaders = event.get("shaders", [])
            output.append(f'{seq}: draw PS={shaders[-1] if shaders else "unknown"} -> {describe(dest)} blend={event.get("blend")} mask={event.get("write_mask")}')
            for source in event.get("inputs", []):
                view = source["view"]
                previous = producers.get(identity(view), "unobserved")
                output.append(f'  stage={source["stage"]} slot={source["backend_slot"]} <- {describe(view)} producer={previous}')
            if event.get("inputs_dropped"):
                output.append(f'  inputs omitted: {event["inputs_dropped"]}')
            producers[identity(dest)] = seq
        elif kind == "compose-color":
            output.append(f'{seq}: compose-color {describe(event.get("source"))} -> {describe(dest)}')
            producers[identity(dest)] = seq
        elif kind == "transfer-request":
            # A virtual operation or stale resolve can suppress actual work.
            output.append(f'{seq}: REQUEST {event["operation"]} {describe(event.get("source"))} -> {describe(dest)} (execution not established)')
        elif kind == "external":
            output.append(f'{seq}: EXTERNAL {describe(dest)} producer={producers.get(identity(dest), "unobserved")}')
        elif kind == "hdr-forward-defer":
            output.append(f'{seq}: DEFER {describe(dest)} (execution or cancellation follows)')
            producers[identity(dest)] = "deferred, execution not established by flow"
        elif kind == "hdr-forward-cancel":
            output.append(f'{seq}: CANCEL {describe(dest)} (deferred draw not executed)')
            producers.pop(identity(dest), None)
        else:
            output.append(f'{seq}: {kind} {describe(dest)}')
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log")
    parser.add_argument("--frame", type=int)
    args = parser.parse_args()
    with open(args.log, encoding="utf-8", errors="replace") as stream:
        events, passes = records(stream)
    if not events:
        parser.exit(1, "No QVFR flow records; run the new DLL with profile=on and profile_details=on.\n")
    frame = args.frame if args.frame is not None else max(e["frame"] for e in events)
    print(f"Frame {frame}: sampled observations, not proof of full-image equivalence; GPU times are whole passes.")
    print("\n".join(summarize(events, passes, frame)))


if __name__ == "__main__":
    main()
