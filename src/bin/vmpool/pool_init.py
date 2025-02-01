#!/usr/bin/env python3

import argparse
import json

import remote_command

def init_pool(file_path):
    try:
        # 使用 with 语句安全打开文件
        with open(file_path, 'r', encoding='utf-8') as file:
            # 加载 JSON 数据
            data = json.load(file)

        # 使用数据示例
        print("JSON 数据内容：")
        print(data)

        for item in data:
            if not 'hostname' in item or not 'datadir' in item or not 'port' in item or not 'maxload' in item:
                raise Exception("Invalid item")

        for item in data:
            remote_command.init_one_instance(item['hostname'], item['port'], item['datadir'], item['maxload'])

    except FileNotFoundError:
        print(f"错误：文件 {file_path} 未找到")
    except json.JSONDecodeError:
        print("错误：文件内容不是有效的 JSON 格式")
    except Exception as e:
        print(f"发生未知错误：{str(e)}")


if __name__ == '__main__':
    parser = argparse.ArgumentParser(
        description='一个处理命令行参数的示例程序',
        epilog='示例: python script.py input.txt -o output --verbose -n 5'
    )

    parser.add_argument(
        'configure',
        type=str,
        help='the host name of instance'
    )

    args = parser.parse_args()
    init_pool(args.configure)


