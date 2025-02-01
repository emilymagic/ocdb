#!/usr/bin/env python3

import heapq
import json
from typing import overload

file_path = "pooldata.json"
data = {}

def get_workers(executors, coordinator, consider_load):
    if 'id' in executors[0]:
        load_factor, new_data = get_request_workers(executors, consider_load)
        if load_factor > 1:
            load_factor2, new_data2 = get_lower_load_workers(executors, consider_load)
            if (load_factor / load_factor2)> 1.8:
                new_data = new_data2
    else:
        load_factor, new_data = get_lower_load_workers(executors, consider_load)

    for key, item in data.items():
        if int(key) == coordinator['id']:
            new_coordinator = item.copy()
            new_coordinator["content"] = -1
            new_coordinator["id"] = coordinator['id']
            new_data.append(new_coordinator)
            break

    return new_data

def get_request_workers(executors, consider_load):
    new_data = []
    load_factor = 0
    i = 0
    for executor in executors:
        item = data["%d" % executor['id']]

        if consider_load:
            item['load'] = item['load'] + 1

        new_load_factor = item['load'] / item['maxload']
        if new_load_factor > load_factor:
            load_factor = new_load_factor

        new_item = {'id': executor['id'],
                    'content': i,
                    'port': item['port'],
                    'hostname': item['hostname'],
                    'datadir': item['datadir'],
                    'load': item['load']}
        new_data.append(new_item)
        i += 1

    return load_factor, new_data

def get_lower_load_workers(executors, consider_load):
    new_data = []
    load_factor = 0
    lower_load_data = get_lower_load_instance(len(executors))
    for i in range(len(lower_load_data)):
        key, item = lower_load_data[i]
        if consider_load:
            item['load'] = item['load'] + 1

        new_load_factor = item['load'] / item['maxload']
        if new_load_factor > load_factor:
            load_factor = new_load_factor

        new_item = {'id': int(key),
                    'content': i,
                    'port': item['port'],
                    'hostname': item['hostname'],
                    'datadir': item['datadir'],
                    'load': item['load']}
        new_data.append(new_item)

    return load_factor, new_data

def get_lower_load_instance(numbers):
    if numbers <= 0:
        return []

    return heapq.nsmallest(numbers, data.items(), key=lambda x: x[1]['load'])

def release_workers(executor, coordinator):
    new_executor = []
    for item in executor:
        ins = data["%d" % item['id']]
        ins['load'] = ins['load'] - 1
        new_executor.append(ins)

    return new_executor


# Persistent instance information
def write_data():
    try:
        # write file
        with open(file_path, 'w', encoding='utf-8') as file:
            # 写入格式化的 JSON（indent=4 增加可读性）
            json.dump(data, file, ensure_ascii=False, indent=4)

        print(f"成功写入 JSON 文件：{file_path}")

    except TypeError as e:
        print(f"数据类型错误：{str(e)}（例如包含 datetime 对象等不可序列化的类型）")
    except PermissionError:
        print(f"权限错误：无法写入 {file_path}")
    except Exception as e:
        print(f"发生未知错误：{str(e)}")

def load_data():
    global data

    try:
        # 使用 with 语句安全打开文件
        with open(file_path, 'r', encoding='utf-8') as file:
            # 加载 JSON 数据
            data = json.load(file)

        # 使用数据示例
        print("JSON 数据内容：")
        print(data)

    except FileNotFoundError:
        print(f"错误：文件 {file_path} 未找到")
    except json.JSONDecodeError:
        print("错误：文件内容不是有效的 JSON 格式")
    except Exception as e:
        print(f"发生未知错误：{str(e)}")

