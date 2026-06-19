################################################################################
# pidvd-player — built from this repo's player/ tree
################################################################################

PIDVD_PLAYER_VERSION = local
PIDVD_PLAYER_SITE = $(BR2_EXTERNAL_PIDVD_PATH)/../player
PIDVD_PLAYER_SITE_METHOD = local
PIDVD_PLAYER_LICENSE = MIT
PIDVD_PLAYER_DEPENDENCIES = libdvdread libdvdnav libdrm alsa-lib libmpeg2 a52dec speexdsp

$(eval $(cmake-package))
