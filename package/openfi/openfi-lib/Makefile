#
# OpenFi 6C 后端公共库
#
# 三个包(风扇 / 模组 / 短信)共用这一套。做成静态库(.a):
#   · 体积很小,三个包各链一份也就几十 KB
#   · 不用管 .so 版本、rpath、ldconfig —— 少一类上机才发现的问题
#
include $(TOPDIR)/rules.mk

PKG_NAME:=openfi-lib
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-only
PKG_MAINTAINER:=OpenFi 6C

include $(INCLUDE_DIR)/package.mk

define Package/openfi-lib
  SECTION:=libs
  CATEGORY:=Libraries
  TITLE:=OpenFi 6C 后端公共库(静态)
  URL:=https://github.com/Nohello-ai/immortalwrt-mt798x-25.12
  DEPENDS:=
endef

define Package/openfi-lib/description
  OpenFi 6C 后端各包共用的基础设施:
    uci 配置读取(自己解析 UCI 文件,不依赖 libuci)
    sysfs 读写、LED 控制、温度采集
    通用分段阈值模板(曲线/信号格数等)
    风扇控制律与状态机
    AT 指令响应解析 + 串口通道(termios)
  全部逻辑都有本地单元测试覆盖。
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		AR="$(TARGET_AR)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		JSON_CFLAGS=""
endef

define Build/InstallDev
	$(INSTALL_DIR) $(1)/usr/include/openfi
	$(CP) $(PKG_BUILD_DIR)/*.h $(1)/usr/include/openfi/
	$(INSTALL_DIR) $(1)/usr/lib
	$(CP) $(PKG_BUILD_DIR)/libopenfi.a $(1)/usr/lib/
endef

$(eval $(call BuildPackage,openfi-lib))
