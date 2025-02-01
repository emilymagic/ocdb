#!/usr/bin/env python3


import json
import os
import requests

import remote_command

def destroy_pool():
    url = "http://127.0.0.1:5000/items"
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
                remote_command.remove_one_instance(item['hostname'], item['datadir'])
        else:
            # 打印错误信息
            print(f"Failed to fetch data. Status code: {response.status_code}")
            print(f"Response: {response.text}")
    except FileNotFoundError:
        print(f"错误：文件 {file_path} 未找到")

    except json.JSONDecodeError:
        print("错误：文件内容不是有效的 JSON 格式")
    except Exception as e:
        print(f"发生未知错误：{str(e)}")

if __name__ == '__main__':
    destroy_pool()
