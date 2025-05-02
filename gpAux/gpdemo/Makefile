# ----------------------------------------------------------------------
# Makefile for Greenplum Demo
# ----------------------------------------------------------------------

#
# INCLUDES
#

top_builddir = ../..
-include $(top_builddir)/src/Makefile.global

#
# VARIABLES
#

PORT_CATALOG ?= 5432
PORT_BASE ?= 7000
NUM_PRIMARY_MIRROR_PAIRS ?= 7
MAX_LOAD ?= 10
HOSTNAME := $(shell hostname | tr -d '\n')
CURRENT_DIR := $(shell pwd)
S3_URL := 'http://127.0.0.1:9000'

export DEMO_PORT_BASE=$(PORT_BASE)
export CATALOG_PORT=5432
export NUM_PRIMARY_MIRROR_PAIRS
export DEFAULT_QD_MAX_CONNECT=150
export CLUSTER_ID=1

all:
	$(MAKE) clean
	$(MAKE) pool
	$(MAKE) catalog

clean:
	$(MAKE) cleanpool
	$(MAKE) cleancatalog

pool:
	sh killpool.sh
	rm -f pooldata.json
	pool_make_conf ${DEMO_PORT_BASE} ${NUM_PRIMARY_MIRROR_PAIRS} ${MAX_LOAD}
	nohup pool &
	pool_init instances.json
	pool_start

cleanpool:
	pool_destroy
	rm -f gpdemo-env.sh

catalog:
	cs_init ${HOSTNAME} ${PORT_CATALOG} ${CURRENT_DIR}/datadirs/catalog ${S3_URL}
	@echo ""

cleancatalog:
	pool_remove_instance ${HOSTNAME} ${CURRENT_DIR}/datadirs/catalog
	rm -f catalog_env.sh
	@echo ""


.PHONY: all cluster create-demo-cluster create-segwalrep-cluster probe clean check clean destroy-demo-cluster
