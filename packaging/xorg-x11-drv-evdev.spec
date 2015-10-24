#sbs-git:slp/pkgs/xorg/driver/xserver-xorg-input-evdev xserver-xorg-input-evdev 2.3.2 1bd95071427e460187b090bc5ff5a2d880fe156a
Name:	xorg-x11-drv-evdev
Summary:    Xorg X11 evdev input driver
Version: 2.7.6.19
Release:    3
Group:      System/X Hardware Support
License:    MIT
URL:        http://www.x.org/
Source0:    %{name}-%{version}.tar.gz
#Requires:   xorg-server
Requires:   xorg-x11-server-Xorg
BuildRequires:  pkgconfig(xorg-macros)
BuildRequires:  pkgconfig(xorg-server)
BuildRequires:  pkgconfig(xkbfile)
BuildRequires:  pkgconfig(inputproto)
BuildRequires:  pkgconfig(libudev)
BuildRequires:  pkgconfig(mtdev)
BuildRequires:  pkgconfig(ttrace)
Requires:  libudev
Requires:  mtdev

%description
The Xorg X11 evdev input driver


%package devel
Summary:    Development files for xorg evdev driver
Group:      Development/Libraries
Requires:   %{name} = %{version}-%{release}

%description devel
This package contains xorg evdev development files


%prep
%setup -q

%build
%autogen

%if "%{?tizen_profile_name}" == "mobile"
export CFLAGS+=" -D_ENV_MOBILE_"
%else
%if "%{?tizen_profile_name}" == "wearable"
export CFLAGS+=" -D_ENV_WEARABLE_ -D_F_SUPPORT_ROTATION_ANGLE_ -D_F_EVDEV_SUPPORT_ROTARY_"
%else
%if "%{?tizen_profile_name}" == "tv"
export CFLAGS+=" -D_ENV_TV_ -D_F_PROXY_DEVICE_ENABLED_ -D_F_EVDEV_SUPPORT_SMARTRC_"
export CFLAGS+=" -D_F_PICTURE_OFF_MODE_ENABLE_ -D_F_DONOT_SEND_RMA_BTN_RELEASE_ -D_F_PROXY_DEVICE_CHANGE_SOURCE_ID"
export CFLAGS+=" -D_F_BLOCK_MOTION_DEVICE_ -D_F_BOOST_PULSE_ -D_F_REMAP_KEYS_ -D_F_SMART_RC_CHG_KBD_SRC_DEV_ "
%endif
%endif
%endif
%reconfigure --disable-static CFLAGS="$CFLAGS -Wall -g -D_F_EVDEV_CONFINE_REGION_ -D_F_ENABLE_DEVICE_TYPE_PROP_ -D_F_GESTURE_EXTENSION_ -D_F_TOUCH_TRANSFORM_MATRIX_ -D_F_ENABLE_REL_MOVE_STATUS_PROP_ -D_F_EVDEV_SUPPORT_GAMEPAD -D_F_USE_DEFAULT_XKB_RULES_ "

make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}/usr/share/license
cp -af COPYING %{buildroot}/usr/share/license/%{name}
%make_install

%remove_docs

%files
%defattr(-,root,root,-)
%(pkg-config xorg-server --variable=moduledir )/input/evdev_drv.so
/usr/share/license/%{name}

%files devel
%defattr(-,root,root,-)
%{_includedir}/xorg/evdev-properties.h
%{_libdir}/pkgconfig/xorg-evdev.pc
