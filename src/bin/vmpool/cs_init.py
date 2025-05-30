#!/usr/bin/env python3

import argparse
import os
import sys
import time
import remote_command
import uuid

def init_catalog_server(hostname, port, datadir, s3_url):
    gp_home = os.getenv("GPHOME")
    if gp_home is None:
        print("PGHOME not set")
        sys.exit(1)

    catalog_server_id = uuid.uuid4()
    remote_command.create_minio_bucket("dbdata-%s" % catalog_server_id.hex, s3_url)
    time.sleep(10)
    cmd = "%s/bin/initdb -D %s" %(gp_home, datadir)
    print("cmd: %s\n" % cmd)
    remote_command.command_run(cmd, hostname)

    remote_command.add_text_to_file(hostname, "%s/postgresql.conf" % datadir, "listen_addresses=\'*\'")
    remote_command.add_text_to_file(hostname, "%s/postgresql.conf" % datadir, "gp_contentid=-1")
    remote_command.add_text_to_file(hostname, "%s/internal.auto.conf" % datadir, "gp_dbid=1")
    remote_command.add_text_to_file(hostname, "%s/internal.auto.conf" % datadir,
                                    "catalog_server_id='%s'" % catalog_server_id.hex)
    remote_command.add_text_to_file(hostname, "%s/internal.auto.conf" % datadir, "s3_url=%s" % s3_url)
    remote_command.add_text_to_file(hostname, "%s/internal.auto.conf" % datadir,
                     "vmpool_url=http://%s:%d" % (remote_command.vmpool_hostname, remote_command.vmpool_port))
    remote_command.add_text_to_file(hostname, "%s/pg_hba.conf" % datadir,
                                    "host    all       all    0.0.0.0/0       trust")
    remote_command.add_text_to_file(hostname, "%s/pg_hba.conf" % datadir,
                                    "host    all       all    ::/0            trust")

    cmd= ("AWS_EC2_METADATA_DISABLED='true' PGOPTIONS='-c gp_role=utility -c default_table_access_method=heap'"
          " PGPORT=%d %s/bin/pg_ctl -D %s -l logfile start") % (port, gp_home, datadir)
    print("cmd: %s\n" % cmd)
    remote_command.command_run(cmd, hostname)

    cmd = ("PGOPTIONS='-c gp_role=utility -c default_table_access_method=heap' %s/bin/psql postgres -c 'select version()'"
           % (gp_home))
    print("cmd: %s\n" % cmd)
    remote_command.command_run(cmd, hostname)

    remote_command.add_text_to_file(hostname, "catalog_env.sh", "export PGPORT=%d" % port)
    remote_command.add_text_to_file(hostname, "catalog_env.sh",
                                    "export PGOPTIONS='-c gp_role=utility -c default_table_access_method=heap'")

if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='init the catalog server',
        epilog='example: cs_init hostname port datadir'
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
        's3_url',
        type=str,
        help='the url of s3'
    )

    args = parser.parse_args()

    init_catalog_server(args.hostname, args.port, args.datadir, args.s3_url)
