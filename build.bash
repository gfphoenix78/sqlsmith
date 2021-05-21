#!/bin/bash

# generate gitrev.h
if git describe --dirty --tags --always > /dev/null ; then \
    echo "#define GITREV \"$(git describe --dirty --tags --always)\"" > gitrev.h ;\
else \
    echo "#define GITREV \"unreleased\"" > gitrev.h ;\
fi

# generate config.h
cat > config.h <<-EOF

/* Define to 1 if you have the 'sqlite3' library (-lsqlite3). */
/* #undef HAVE_LIBSQLITE3 */

/* define if the MonetDB client library is available */
/* #undef HAVE_MONETDB */

/* Name of package */
#define PACKAGE "sqlsmith"

/* Define to the address where bug reports for this package should be sent. */
#define PACKAGE_BUGREPORT "seltenreich@gmx.de"

/* Define to the full name of this package. */
#define PACKAGE_NAME "SQLsmith"

/* Define to the full name and version of this package. */
#define PACKAGE_STRING "SQLsmith 1.2.1"

/* Define to the one symbol short name of this package. */
#define PACKAGE_TARNAME "sqlsmith"

/* Define to the home page for this package. */
#define PACKAGE_URL "https://github.com/anse1/sqlsmith/"

/* Define to the version of this package. */
#define PACKAGE_VERSION "1.2.1"

/* Version number of package */
#define VERSION "1.2.1"

EOF

function sqlsmith() {
make -f Makefile.pg -j8
}
function clean() {
make -f Makefile.pg clean
}

$*
