#!/usr/bin/env python3

import argparse

import remote_command


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='init the instance',
        epilog='example: pool_init_instance hostname port datadir maxload'
    )

    parser.add_argument(
        'hostname',
        type=str,
        help='the host name of instance'
    )

    parser.add_argument(
        'port',
        type=int,
        help='the port of instance'
    )

    parser.add_argument(
        'datadir',
        type=str,
        help='the data directory of instance'
    )

    parser.add_argument(
        'maxload',
        type=int,
        help='the max backend for instance'
    )

    args = parser.parse_args()

    remote_command.init_one_instance(args.hostname, args.port, args.datadir, args.maxload)
