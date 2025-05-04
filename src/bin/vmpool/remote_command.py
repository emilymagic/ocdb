#!/usr/bin/env python3

import json
import os
import sys
import requests
import paramiko
import boto3
from botocore.exceptions import ClientError
import socket

vmpool_hostname = os.getenv('VMPOOLHOST')
if vmpool_hostname is None:
    vmpool_hostname = socket.gethostname()
vmpool_port = 5000

def command_run(command, hostname):
    # 创建SSH客户端
    ssh = paramiko.SSHClient()

    # 自动添加主机密钥（如果不这样做，第一次连接时会提示确认）
    ssh.set_missing_host_key_policy(paramiko.AutoAddPolicy())

    try:
        # 连接到远程服务器
        ssh.connect(hostname)

        # 执行命令
        stdin, stdout, stderr = ssh.exec_command(command)

        # 读取命令输出
        output = stdout.read().decode()
        error = stderr.read().decode()

        # 打印输出
        if output:
            print("Output:")
            print(output)
        if error:
            print("Error:")
            print(error)

    finally:
        # 关闭连接
        ssh.close()

def add_text_to_file(hostname, filename, text):
    """
    在文件中添加文本
    :param filename: 目标文件名
    :param text: 要添加的文本
    :param position: 添加位置 ('beginning' 或 'end')
    """
    try:

        cmd = "echo \"%s\" >> %s" % (text, filename)
        command_run(cmd, hostname)

    except FileNotFoundError:
        print(f"错误: 文件 {filename} 不存在")
    except PermissionError:
        print(f"错误: 没有写入 {filename} 的权限")
    except Exception as e:
        print(f"发生未知错误: {str(e)}")

def init_one_instance(hostname, port, datadir, maxload):
    url = "http://%s:%s/items" % (vmpool_hostname, vmpool_port)
    try:
        gp_home = os.getenv("GPHOME")
        if gp_home is None:
            print("PGHOME not set")
            sys.exit(1)

        print("url: %s\n" % url)


        # init instance
        cmd = "%s/bin/initdb -J segment -E UTF-8 -D %s" % (gp_home, datadir)
        print("cmd: %s\n" % cmd)
        command_run(cmd, hostname)

        # copy pg_internal to data dir
        cmd = "cp %s/share/local_pg_internal.init %s/local_pg_internal.init" % (gp_home, datadir)
        print("cmd: %s\n" % cmd)
        command_run(cmd, hostname)

        cmd = "cp %s/share/global_pg_internal.init %s/global/pg_internal.init" % (gp_home, datadir)
        print("cmd: %s\n" % cmd)
        command_run(cmd, hostname)

        new_item = {"port": port,
                    "hostname": hostname,
                    "datadir": datadir,
                    'maxload': maxload}
        headers = {"Content-Type": "application/json"}
        response = requests.post(
            url,
            json=new_item,
            headers=headers
        )

        if response.status_code == 201:
            print("添加成功:", response.json())
            ret = response.json()
        else:
            raise Exception("Add to pool error %d", response.status_code)

        add_text_to_file(hostname, "%s/postgresql.conf" % datadir, "listen_addresses=\'*\'")
        # add_text_to_file("%s/postgresql.conf" % datadir, "gp_contentid=%d" % contentid)
        add_text_to_file(hostname, "%s/postgresql.conf" % datadir, "fsync=off")
        add_text_to_file(hostname, "%s/postgresql.conf" % datadir, "port=%d" % port)
        add_text_to_file(hostname, "%s/internal.auto.conf" % datadir, "gp_dbid=%d" % ret['id'])
        add_text_to_file(hostname, "%s/internal.auto.conf" % datadir, "cluster_id=1")
        add_text_to_file(hostname, "%s/internal.auto.conf" % datadir,
                         "vmpool_url=http://%s:%d" % (vmpool_hostname, vmpool_port))

    except requests.exceptions.RequestException as e:
        # 处理请求异常
        print(f"An error occurred: {e}")

def remove_one_instance(hostname, datadir):
    gp_home = os.getenv("GPHOME")
    cmd = "%s/bin/pg_ctl stop -D %s " % (gp_home, datadir)
    command_run(cmd, hostname)

    try:
        url = "http://%s:%s/itemsdelete" % (vmpool_hostname, vmpool_port)

        new_item = {"hostname": hostname,
                    "datadir": datadir}
        headers = {"Content-Type": "application/json"}
        response = requests.post(
            url,
            json=new_item,
            headers=headers
        )

        if response.status_code == 200:
            print("Remove instance success: %s", response.json())
        else:
            print("Remove instance failed, status code %d", response.status_code)
        cmd = "rm -r %s" % (datadir)
        command_run(cmd, hostname)
    except FileNotFoundError:
        print(f"错误：文件 {file_path} 未找到")
    except json.JSONDecodeError:
        print("错误：文件内容不是有效的 JSON 格式")
    except Exception as e:
        print(f"发生未知错误：{str(e)}")

def create_minio_bucket(bucket_name):
    # 连接到 MinIO 服务器
    s3_client = boto3.client(
        "s3",
        endpoint_url="http://192.168.103.130:9000",  # MinIO 服务器地址和端口
        region_name="us-east-1",  # 可任意指定（MinIO 不强制要求）
        aws_access_key_id = "minioadmin",  # MINIO_ROOT_USER
        aws_secret_access_key = "minioadmin"  # MINIO_ROOT_PASSWORD
    )

    try:
        # 创建 Bucket
        s3_client.create_bucket(Bucket=bucket_name)
        print(f"Bucket '{bucket_name}' 创建成功！")
    except ClientError as e:
        if e.response["Error"]["Code"] == "BucketAlreadyExists":
            print(f"Bucket '{bucket_name}' 已存在。")
        else:
            print(f"错误: {e}")
