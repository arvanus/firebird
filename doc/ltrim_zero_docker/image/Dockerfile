# Firebird 5.0.3 with the standalone LTRIM_ZERO collation module.
#
# Build context needs fbltrimzero.so and fbltrimzero.conf, both produced by
# ../build_module.sh:
#
#   cp out/fbltrimzero.so out/fbltrimzero.conf doc/ltrim_zero_docker/image/
#   docker build -t srs/firebird:5.0.3-noble-ltrimzero doc/ltrim_zero_docker/image
#
# The base image is untouched apart from the two files. fbintl.conf is NOT
# edited: the engine scans every *.conf in the intl directory
# (Jrd::IntlManager::initialize -> ScanDir(intlPath, "*.conf"),
# src/jrd/IntlManager.cpp:425), so the module registers itself.
#
# In each database that needs it:
#
#   CREATE COLLATION ISO8859_1_LTRIM_ZERO FOR ISO8859_1
#       FROM EXTERNAL ('ISO8859_1_LTRIM_ZERO') CASE INSENSITIVE PAD SPACE;

FROM firebirdsql/firebird:5.0.3-noble

COPY fbltrimzero.so   /opt/firebird/intl/fbltrimzero.so
COPY fbltrimzero.conf /opt/firebird/intl/fbltrimzero.conf

RUN chmod 755 /opt/firebird/intl/fbltrimzero.so \
 && chmod 644 /opt/firebird/intl/fbltrimzero.conf
