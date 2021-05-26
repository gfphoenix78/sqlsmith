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
install_deps

pushd ${CWDIR}
bash build.bash sqlsmith
popd
}
function test_run() {
  prepare_sqlsmith
  cat > /home/gpadmin/test_run.sh <<-EOF
#!/bin/bash -l
set -exo pipefail
source /usr/local/greenplum-db-devel/greenplum_path.sh
source ${TOP_DIR}/gpdb_src/gpAux/gpdemo/gpdemo-env.sh

pushd ${CWDIR}
createdb sqlsmith
ldd ./sqlsmith | grep libpq

./sqlsmith --verbose --target="port=\$PGPORT host=/tmp dbname=sqlsmith" --max-queries=10000
popd
EOF

  chown -R gpadmin:gpadmin $(pwd)
  pushd /home/gpadmin
    chown gpadmin:gpadmin  test_run.sh
    chmod a+x  test_run.sh
  popd
  su gpadmin -c "/bin/bash /home/gpadmin/test_run.sh"
}

function setup_gpadmin_user() {
    ${GPDB_CONCOURSE_DIR}/setup_gpadmin_user.bash
}

function _main() {
    time install_gpdb
    time setup_gpadmin_user
    time make_cluster

    time test_run
}

_main "$@"
