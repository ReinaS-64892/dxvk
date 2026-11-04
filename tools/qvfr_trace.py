#!/usr/bin/env python3
"""Summarize DXVK QVFR draw logs. No stereo or pass classification is inferred."""

import argparse
import collections
import json
import sys


def summarize(lines):
    targets = {}
    records = 0
    for line_number, line in enumerate(lines, 1):
        marker = line.find('QVFR {')
        if marker < 0:
            continue
        try:
            draw = json.loads(line[marker + len('QVFR '):])
            if draw['version'] != 1:
                raise ValueError('unsupported QVFR version')
            colors, depth = draw['colors'], draw['depth']
            # Include subresources and formats: a resource can have multiple views.
            key = json.dumps([draw['width'], draw['height'], draw['layers'], colors, depth],
                             sort_keys=True)
            if key not in targets:
                targets[key] = dict(width=draw['width'], height=draw['height'],
                                    layers=draw['layers'], colors=colors, depth=depth,
                                    records=0, draw_count_or_upper_bound=0,
                                    indirect_records=0, storage_records=0, xfb_records=0,
                                    layer_export_records=0, passes=set(), shaders=set(),
                                    viewports=set(), instance_counts=collections.Counter())
            target = targets[key]
            target['records'] += 1
            target['draw_count_or_upper_bound'] += draw['count']
            target['indirect_records'] += draw['instances'] is None
            target['storage_records'] += bool(draw['storage'])
            target['xfb_records'] += bool(draw['xfb'])
            target['layer_export_records'] += bool(draw['layer_export'])
            target['passes'].add((draw['context'], draw['pass']))
            target['shaders'].add(tuple(draw['shaders']))
            target['viewports'].add(json.dumps(draw['viewports'], sort_keys=True))
            if draw['instances'] is not None:
                target['instance_counts'][draw['instances']] += 1
            records += 1
        except (ValueError, KeyError, TypeError) as error:
            raise ValueError(f'line {line_number}: malformed QVFR record: {error}') from error

    result = []
    for target in targets.values():
        target['passes'] = len(target['passes'])
        target['shaders'] = sorted(target['shaders'], key=lambda s: json.dumps(s))
        target['viewports'] = [json.loads(v) for v in sorted(target['viewports'])]
        target['instance_counts'] = dict(sorted(target['instance_counts'].items()))
        result.append(target)
    result.sort(key=lambda t: (-t['records'], t['width'], t['height']))
    return dict(records=records, targets=result)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('log', nargs='?', help='DXVK log file; default: stdin')
    args = parser.parse_args()
    try:
        if args.log:
            with open(args.log, encoding='utf-8', errors='replace') as stream:
                result = summarize(stream)
        else:
            result = summarize(sys.stdin)
        if not result['records']:
            raise ValueError('no QVFR draw records found; enable DXVK_QVFR_TRACE=1 and info logging')
        json.dump(result, sys.stdout, indent=2)
        print()
    except (OSError, ValueError) as error:
        parser.exit(2, f'{error}\n')


if __name__ == '__main__':
    main()
