# Firebird 5.0.3 with the standalone ID_ZPAD_CI collation module.
#
# Build context needs lrsintl.so and lrsintl.conf, both produced by
# ../build_module.sh:
#
#   cp out/lrsintl.so out/lrsintl.conf doc/lrsintl_docker/image/
#   docker build -t srs/firebird:5.0.3-noble-lrsintl doc/lrsintl_docker/image
#
# The base image is untouched apart from the two files. fbintl.conf is NOT
# edited: the engine scans every *.conf in the intl directory
# (Jrd::IntlManager::initialize -> ScanDir(intlPath, "*.conf"),
# src/jrd/IntlManager.cpp:425), so the module registers itself.
#
# In each database that needs it:
#
#   CREATE COLLATION ISO8859_1_ID_ZPAD_CI FOR ISO8859_1
#       FROM EXTERNAL ('ISO8859_1_ID_ZPAD_CI') CASE INSENSITIVE PAD SPACE;

FROM firebirdsql/firebird:5.0.3-noble

COPY lrsintl.so   /opt/firebird/intl/lrsintl.so
COPY lrsintl.conf /opt/firebird/intl/lrsintl.conf

RUN chmod 755 /opt/firebird/intl/lrsintl.so \
 && chmod 644 /opt/firebird/intl/lrsintl.conf
