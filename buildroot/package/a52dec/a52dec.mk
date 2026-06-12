################################################################################
# a52dec (liba52) — AC-3 decoder, local package (not in Buildroot 2025.02)
################################################################################

A52DEC_VERSION = 0.7.4
A52DEC_SITE = http://ftp.debian.org/debian/pool/main/a/a52dec
A52DEC_SOURCE = a52dec_$(A52DEC_VERSION).orig.tar.gz
A52DEC_LICENSE = GPL-2.0+
A52DEC_INSTALL_STAGING = YES
A52DEC_CONF_OPTS = CFLAGS="$(TARGET_CFLAGS) -fPIC"

$(eval $(autotools-package))
