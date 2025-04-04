#!/usr/bin/env python3

import argparse
import json
import pathlib
import socket
from pathlib import Path


import remote_command


def make_pool_conf(baseport, number, maxload):
    current_path = pathlib.Path.cwd()
    print(f"Path对象形式: {current_path}")
    hostname = socket.gethostname()
    print(f"Hostname: {hostname}")
    data = []
    for i in range(number):
        item = {'datadir': "%s/datadirs/vmpool/dbfast%d" % (current_path, i + 1),
                'hostname': hostname, 'port': baseport + i + 2, 'maxload': maxload}
        data.append(item)

    with open('instances.json', 'w', encoding='utf-8') as file:
        # 写入格式化的 JSON（indent=4 增加可读性）
        json.dump(data, file, ensure_ascii=False, indent=4)

    current_working_dir = Path.cwd()
    filename = "%s/gpdemo-env.sh" % current_working_dir
    remote_command.add_text_to_file(hostname, filename, "export PGPORT=%d" % (baseport + 2))

if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='make the pool configure file',
        epilog='example: pool_make_conf hostname port datadir contentid'
    )

    parser.add_argument(
        'baseport',
        type=int,
        help='the port of instance'
    )

    parser.add_argument(
        'number',
        type=int,
        help='the numbers of instance'
    )

    parser.add_argument(
        'maxload',
        type=int,
        help='the numbers of instance'
    )

    args = parser.parse_args()

    make_pool_conf(args.baseport, args.number, args.maxload)
