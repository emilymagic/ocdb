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
```

### Install the dependency of python
```
sudo apt install python3-venv

python3 -m venv venv

source venv/bin/activate

pip install Flask

pip install paramiko

pip install requests
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
