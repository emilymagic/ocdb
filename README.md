## Overview

The octopus is a cloud-native serverless database. You can easily create a
database service on cloud.

## Build environment on Ubuntu
### Install the dependency

```
sudo apt install -y libssl-dev

sudo apt install -y libpulse-dev

sudo apt install -y uuid-dev

sudo apt install -y libcurl4-openssl-dev

sudo apt install -y zlib1g.dev

sudo apt install -y zlib1g

sudo apt install -y zip unzip

sudo apt install -y libreadline-dev

sudo apt install -y libxml2-dev

sudo apt install -y libbzstd-dev

sudo apt install -y libperl-dev

sudo apt install -y bison

sudo apt install -y flex

sudo apt install -y gcc

sudo apt install -y g++

sudo apt install -y cmake

sudo apt install -y python3-dev

sudo apt install -y python3-pip

sudo apt install -y python3-psycopg2

sudo apt install -y python3-psutil

sudo apt install -y libbz2-dev

sudo apt install -y libcjson-dev
```

### Install the dependency of python
```
sudo apt install -y python3-venv

python3 -m venv venv

source venv/bin/activate

pip install Flask

pip install paramiko

pip install requests

pip install boto3
```

### Build install s3
```
git clone git@github.com:aws/aws-sdk-cpp.git
cd aws-sdk-cpp
sh prefetch_crt_dependency.sh
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_TESTING=OFF -DBUILD_ONLY="s3" 
make -j8
sudo make install
```

### Run minio
```
mkdir minio
cd minio
wget https://dl.min.io/server/minio/release/linux-arm64/minio
chmod 755 minio
cd ~
nohup ./minio/minio server ./minio/data &
```

### Add some path to .bashrc
```
ulimit -c unlimited
export AWS_EC2_METADATA_DISABLED='true'
export MINIO_ROOT_USER=minioadmin
export MINIO_ROOT_PASSWORD=minioadmin
```


## Make source code
```
CFLAGS=-ggdb3 CFLAGS="-O0 -g3" ./configure --prefix=/home/gpadmin/gpdb --with-pgport=5432 --enable-cassert --with-perl --with-python --with-libxml --with-openssl --without-zstd --enable-debug-extensions --disable-gpcloud --disable-orca --disable-gpfdist

make -j8 install
```


## Build the database

```
# Bring in greenplum environment into your running shell
source ~/gpdb/greenplum_path.sh
. ~/venv/bin/activate

# Start demo cluster
cd gpAux/gpdemo
make
source gpdemo-env.sh
```

## Running tests
```
cd src/test/regress_tile
make installcheck-parallel
```

## Make a real distributed cluster

### Configuring the installation environment
1. init 2 hosts with Ubuntu
2. make and install the codes in each host
3. Configure password-free ssh access between two machines
4. Update the host file
```
192.168.103.129 gpadmin
192.168.103.130 gpadmin2
```

### deploy the vmpool in 2 hosts gpadmin and gpadmin2
```
source ~/gpdb/greenplum_path.sh
. ~/venv/bin/activate
nohup pool &
pool_init_instance gpadmin 7002 /home/gpadmin/ocdb/gpAux/gpdemo/datadirs/vmpool/dbfast1 10
pool_init_instance gpadmin 7003 /home/gpadmin/ocdb/gpAux/gpdemo/datadirs/vmpool/dbfast2 10
pool_init_instance gpadmin2 7004 /home/gpadmin/ocdb/gpAux/gpdemo/datadirs/vmpool/dbfast1 10
```

### deploy the 2 catalog server
```
cs_init gpadmin 5432 /home/gpadmin/ocdb/gpAux/gpdemo/datadirs/catalog http://gpadmin:9000
cs_init gpadmin 5433 /home/gpadmin/ocdb/gpAux/gpdemo/datadirs/catalog2 http://gpadmin:9000
```

### access the first catalog server
```
export PGPORT=7002
PGOPTIONS='-c catalog_server_host=gpadmin -c catalog_server_port=5432' psql -l
```

### access the second catalog server
```
export PGPORT=7003
PGOPTIONS='-c catalog_server_host=gpadmin -c catalog_server_port=5433' psql -l
```

