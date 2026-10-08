include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=kmod-slic-mt7621
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

define KernelPackage/slic-spi-cs1
  SUBMENU:=Other modules
  TITLE:=Register Le9641 SLIC spidev on MT7621 SPI0 CS1
  DEPENDS:=@TARGET_ramips_mt7621 +kmod-spi-dev
  FILES:=$(PKG_BUILD_DIR)/slic_spi_cs1.ko
endef

define KernelPackage/mt7621-pcm-raw
  SUBMENU:=Other modules
  TITLE:=Minimal MT7621 PCM master with PIO sample pipe (SLIC bring-up)
  DEPENDS:=@TARGET_ramips_mt7621
  FILES:=$(PKG_BUILD_DIR)/mt7621_pcm_raw.ko
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,slic-spi-cs1))
$(eval $(call KernelPackage,mt7621-pcm-raw))
