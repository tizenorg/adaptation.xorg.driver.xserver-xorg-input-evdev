Name:       xorg-input-evdev
Summary:    Xorg evdev input driver
Version:    2.3.2
Release:    5
Group:      System/X Hardware Support
License:    MIT
URL:        http://www.x.org/
Source0:    http://xorg.freedesktop.org/releases/individual/driver/xf86-input-evdev-%{version}.tar.gz
Source1001: packaging/xorg-input-evdev.manifest 
Requires:   xserver-xorg-core
BuildRequires:  pkgconfig(xorg-server)
BuildRequires:  pkgconfig(xkbfile)
BuildRequires:  pkgconfig(xproto)
BuildRequires:  pkgconfig(inputproto)
BuildRequires:  pkgconfig(xrandr)
BuildRequires:  pkgconfig(randrproto)
BuildRequires:  pkgconfig(xextproto)
BuildRequires:  pkgconfig(xorg-macros)


%description
The Xorg X11 evdev input driver


%package devel
Summary:    Development files for xorg evdev driver
Group:      Development/Libraries
Requires:   %{name} = %{version}-%{release}

%description devel
This package contains xorg evdev development files


%prep
%setup -q -n xf86-input-evdev-%{version}

%build
cp %{SOURCE1001} .

%reconfigure --disable-static
make %{?jobs:-j%jobs}

%install
%make_install

%remove_docs

%files
%manifest xorg-input-evdev.manifest
%(pkg-config xorg-server --variable=moduledir )/input/evdev_drv.so

%files devel
%manifest xorg-input-evdev.manifest
%{_includedir}/xorg/evdev-properties.h
%{_libdir}/pkgconfig/xorg-evdev.pc

