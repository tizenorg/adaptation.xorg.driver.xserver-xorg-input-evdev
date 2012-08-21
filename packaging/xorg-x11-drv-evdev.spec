#sbs-git:slp/pkgs/xorg/driver/xserver-xorg-input-evdev xserver-xorg-input-evdev 2.3.2 1bd95071427e460187b090bc5ff5a2d880fe156a
Name:	xorg-x11-drv-evdev
Summary:    Xorg X11 evdev input driver
Version: 2.3.2
Release:    4
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

%reconfigure --disable-static CFLAGS="$CFLAGS -Wall -g -D_F_INIT_ABS_ONLY_FOR_POINTER_ -D_F_EVDEV_CONFINE_REGION_"
make %{?jobs:-j%jobs}

%install
rm -rf %{buildroot}
%make_install

%remove_docs

%files
%defattr(-,root,root,-)
%(pkg-config xorg-server --variable=moduledir )/input/evdev_drv.so

%files devel
%defattr(-,root,root,-)
%{_includedir}/xorg/evdev-properties.h
%{_libdir}/pkgconfig/xorg-evdev.pc

