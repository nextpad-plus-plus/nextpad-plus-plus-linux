Name:           nextpad-plus-plus
Version:        1.0.6
Release:        1%{?dist}
Summary:        Multi-tab text editor (Linux port of Notepad++)

License:        GPLv3+
URL:            https://nextpad.org
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.20
BuildRequires:  pkgconfig(gtk+-3.0)
BuildRequires:  pkgconfig(glib-2.0)

%description
Nextpad++ is a native Linux port of the Notepad++ text editor, built on
Scintilla and Lexilla with GTK 3. Features multi-tab editing, syntax
highlighting for 50+ languages, find/replace with regex, macro
recording, session save/restore, auto-backup, side panels, bookmark
margin, 5-colour mark/style system, command palette, and a plugin SDK.

%prep
%autosetup

%build
%cmake -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%doc README.md NOTICE
%{_bindir}/nextpad-plus-plus
%{_datadir}/applications/org.nextpad.NextpadPP.desktop
%{_datadir}/metainfo/org.nextpad.NextpadPP.metainfo.xml
%{_datadir}/nextpad-plus-plus/
%{_mandir}/man1/nextpad-plus-plus.1*

%changelog
* Wed May 13 2026 Andrey Letov <aletik@gmail.com> - 1.0.6-1
- Initial Linux release.
