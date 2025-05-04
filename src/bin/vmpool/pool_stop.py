#!/usr/bin/env python3

import os
import sys
import requests

import remote_command

def stop_all():
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
                cmd = "%s/bin/pg_ctl stop -D %s " % (gp_home, item["datadir"])
                remote_command.command_run(cmd, item["hostname"])
        else:
            # 打印错误信息
            print(f"Failed to fetch data. Status code: {response.status_code}")
            print(f"Response: {response.text}")

    except requests.exceptions.RequestException as e:
        # 处理请求异常
        print(f"An error occurred: {e}")


if __name__ == '__main__':
    stop_all()