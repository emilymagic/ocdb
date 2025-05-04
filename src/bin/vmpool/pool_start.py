#!/usr/bin/env python3

import os
import sys
import requests

import remote_command

def start_all():
    url = "http://%s:%s/items" % (remote_command.vmpool_hostname, remote_command.vmpool_port)
    try:
        gp_home = os.getenv("GPHOME")
        if gp_home is None:
            print("PGHOME not set")
            sys.exit(1)

        # 发送GET请求
        response = requests.get(url)

        # 检查请求是否成功
        if response.status_code == 200:
            # 解析JSON响应
            items = response.json()

            # 打印所有数据
            print("Items returned from the server:")
            for item in items.values():
                #cmd = "AWS_EC2_METADATA_DISABLED='true' nohup %s/bin/postgres -D %s -c gp_role=dispatch &" % (gp_home, item["datadir"])
                cmd = ("AWS_EC2_METADATA_DISABLED='true' %s/bin/pg_ctl -l %s/log/startup.log -D %s -o \"-p %d -c gp_role=dispatch\" start" %
                       (gp_home, item["datadir"], item["datadir"], item["port"]))
                remote_command.command_run(cmd, item["hostname"])

        else:
            # 打印错误信息
            print(f"Failed to fetch data. Status code: {response.status_code}")
            print(f"Response: {response.text}")

    except requests.exceptions.RequestException as e:
        # 处理请求异常
        print(f"An error occurred: {e}")

if __name__ == '__main__':
    start_all()
