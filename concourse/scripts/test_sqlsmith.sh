#!/bin/bash -l
set -exo pipefail

OLDPATH=${PATH}
echo "OLDPATH = ${OLDPATH}"
CWDIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
TOP_DIR=${CWDIR}/../../../
GPDB_CONCOURSE_DIR=${TOP_DIR}/gpdb_src/concourse/scripts

source "${GPDB_CONCOURSE_DIR}/common.bash"

function install_deps() {
yum install -y libpqxx-devel
}
function prepare_sqlsmith() {
pushd ${CWDIR}/../../
bash build.bash sqlsmith
popd
}
function test_run() {
  cat > /home/gpadmin/test_run.sh <<-EOF
#!/bin/bash -l
set -exo pipefail
source /usr/local/greenplum-db-devel/greenplum_path.sh
source ${TOP_DIR}/gpdb_src/gpAux/gpdemo/gpdemo-env.sh

pushd ${CWDIR}/../../
createdb sqlsmith
ldd ./sqlsmith | grep libpq

./sqlsmith --verbose --target="port=\$PGPORT host=/tmp dbname=sqlsmith" --max-queries=10000 && touch /tmp/pipeline_status_OK || echo "pipeline failed"

popd
EOF

  pushd /home/gpadmin
    chown gpadmin:gpadmin  test_run.sh
    chmod a+x  test_run.sh
  popd
  su gpadmin -c "/bin/bash /home/gpadmin/test_run.sh"

  ls /tmp/core.postgres* 2>/dev/null && has_core=yes || echo "No core dump files"
  if [ "$has_core" == "yes" ]; then
    local coredump=coredump-$(date +"%F--%H-%M-%S").tar
    pushd ${TOP_DIR}/bin_gpdb
    tar cf ~/$coredump bin_gpdb.tar.gz
    popd

    pushd /tmp
    for cc in core.postgres* ;
    do
        tar rf ~/$coredump $cc
    done
    # attach log of master node
    pushd ${TOP_DIR}/gpdb_src/gpAux/gpdemo/datadirs/qddir/demoDataDir-1/
    [ -d "pg_log" ] && tar rf ~/$coredump pg_log
    [ -d "log" ] && tar rf ~/$coredump log
    popd

    mv ~/$coredump ${TOP_DIR}/sqlsmith/ 
    echo "@@@@ Found core dump files @@@@"
  fi
  [ -f /tmp/pipeline_status_OK ] || exit 1
  exit 0
}

function setup_gpadmin_user() {
    ${GPDB_CONCOURSE_DIR}/setup_gpadmin_user.bash
}

function _main() {
    time install_deps
    time prepare_sqlsmith

    time install_gpdb
    time setup_gpadmin_user
    time make_cluster

    time test_run
}

_main "$@"
