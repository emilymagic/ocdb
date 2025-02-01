#!/usr/bin/env python3

import argparse
import remote_command

if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='一个处理命令行参数的示例程序',
        epilog='示例: python script.py input.txt -o output --verbose -n 5'
    )

    parser.add_argument(
        'hostname',
        type=str,
        help='the host name of instance'
    )

    parser.add_argument(
        'datadir',
        type=str,
        help='the data directory of instance'
    )

    args = parser.parse_args()

    remote_command.remove_one_instance(args.hostname, args.datadir)
