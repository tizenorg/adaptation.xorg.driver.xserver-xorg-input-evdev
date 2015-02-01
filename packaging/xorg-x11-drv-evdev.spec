#sbs-git:slp/pkgs/xorg/driver/xserver-xorg-input-evdev xserver-xorg-input-evdev 2.3.2 1bd95071427e460187b090bc5ff5a2d880fe156a
Name:	xorg-x11-drv-evdev
Summary:    Xorg X11 evdev input driver
Version: 2.7.6.7
Release:    3
Group:      System/X Hardware Support
License:    MIT
URL:        http://www.x.org/
Source0:    %{name}-%{version}.tar.gz
#Requires:   xorg-server
Requires:   xorg-x11-server-Xorg
BuildRequires:  pkgconfig(xorg-macros)
BuildRequires:  xorg-x11-server-devel
#BuildRequires:  pkgconfig(xorg-server)
BuildRequires:  pkgconfig(xkbfile)
BuildRequires:  pkgconfig(xproto)
BuildRequires:  pkgconfig(inputproto)
BuildRequires:  pkgconfig(xrandr)
BuildRequires:  pkgconfig(randrproto)
BuildRequires:  pkgconfig(xextproto)
BuildRequires:  pkgconfig(resourceproto)
BuildRequires:  pkgconfig(libudev)
BuildRequires:  pkgconfig(mtdev)
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
