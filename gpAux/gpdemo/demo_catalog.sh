#!/usr/bin/env bash

cleanCatalog() 
{
    cmd="$GPHOME/bin/pg_ctl"
    cmd="$cmd -D ${CATALOGDIR}"
    cmd="$cmd stop"    
    $cmd

    if [ -f ${CATALOG_CONFIG} ];
    then
        echo "Deleting catalog configure file"
        rm -f ${CATALOG_CONFIG}
    fi
    if [ -d ${CATALOGDIR} ]
    then
        echo "Deleting ${CATALOGDIR}"
        rm -rf ${CATALOGDIR}
    fi
    
    if [ -f ${CATALOG_ENV} ]
    then
        echo "Deleting ${CATALOG_ENV}"
        rm -rf ${CATALOG_ENV}
    fi
    `clearbucket`
}

if [ -z "${COORDINATOR_DATADIR}" ]; then
  DATADIRS=${DATADIRS:-`pwd`/datadirs}
else
  DATADIRS="${COORDINATOR_DATADIR}/datadirs"
fi

CATALOGDIR=$DATADIRS/catalog
CATALOG_CONFIG=catalogConfigFile
CATALOG_ENV=catalog-env.sh

#*****************************************************************************
# Main Section
#*****************************************************************************

while getopts ":d'?'" opt
do
        case $opt in
                '?' ) USAGE ;;
        d) cleanCatalog
           exit 0
           ;;
        *) USAGE
           exit 0
           ;;
        esac
done


rm -f ${CATALOG_CONFIG}
cat >> $CATALOG_CONFIG <<-EOF
	# Name of directory on that host in which to setup the QD
	CATALOG_DIRECTORY=$CATALOGDIR
EOF

GPPATH=`find -H $GPHOME -name gpstart| tail -1`
cmd='gpinitcatalog'
$cmd

cat > ${CATALOG_ENV} <<-EOF
	## ======================================================================
	##                                gpdemo
	## ----------------------------------------------------------------------
	## timestamp: $( date )
	## ======================================================================

	export PGPORT=${CATALOG_PORT}
	export PGOPTIONS='-c gp_role=utility -c default_table_access_method=heap'
EOF

