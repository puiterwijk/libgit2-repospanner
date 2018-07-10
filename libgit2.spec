Name:           libgit2
Version:        0.26.5
Release:        1%{?dist}
Summary:        C implementation of the Git core methods as a library with a solid API
License:        GPLv2 with exceptions
URL:            http://libgit2.github.com/
Source0:        https://github.com/libgit2/libgit2/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  cmake
BuildRequires:  http-parser-devel
BuildRequires:  libcurl-devel
BuildRequires:  libssh2-devel
BuildRequires:  openssl-devel
BuildRequires:  python2
BuildRequires:  zlib-devel
BuildRequires:  libgit2
Provides:       bundled(libxdiff)

%description
libgit2 is a portable, pure C implementation of the Git core methods 
provided as a re-entrant linkable library with a solid API, allowing
you to write native speed custom Git applications in any language
with bindings.

%package        devel
Summary:        Development files for %{name}
Requires:       %{name}%{?_isa} = %{?epoch:%{epoch}:}%{version}-%{release}

%description    devel
This package contains libraries and header files for
developing applications that use %{name}.

%prep
%autosetup -p1

# Remove VCS files from examples
find examples -name ".gitignore" -delete -print

# Fix pkgconfig generation
sed -i 's|@CMAKE_INSTALL_PREFIX@/||' libgit2.pc.in

# Don't test network
sed -i 's/ionline/xonline/' CMakeLists.txt

# Remove bundled libraries
rm -frv deps

%build
mkdir %{_target_platform}
pushd %{_target_platform}
  %cmake -DTHREADSAFE=ON ..
popd
%make_build -C %{_target_platform}

%install
%make_install -C %{_target_platform}

# Include previous ABI version for temporary binary compatibility
cp -a %{_libdir}/libgit2.so.*24* $RPM_BUILD_ROOT%{_libdir}

%check
pushd %{_target_platform}
  ctest -VV
popd

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
%license COPYING
%{_libdir}/%{name}.so.*

%files devel
%doc AUTHORS docs examples README.md
%{_libdir}/%{name}.so
%{_libdir}/pkgconfig/%{name}.pc
%{_includedir}/git2.h
%{_includedir}/git2/

%changelog
* Tue Jul 10 2018 Pete Walter <pwalter@fedoraproject.org> - 0.26.5-1
- Update to 0.26.5 (CVE-2018-10887, CVE-2018-10888)

* Mon Jun 25 2018 Pete Walter <pwalter@fedoraproject.org> - 0.26.4-1
- Update to 0.26.4 (CVE-2018-11235)

* Mon Apr 30 2018 Pete Walter <pwalter@fedoraproject.org> - 0.26.3-1
- Update to 0.26.3
- Include previous ABI version for temporary binary compatibility

* Thu Aug 10 2017 Pete Walter <pwalter@fedoraproject.org> - 0.24.6-2
- Drop 0.21.5 ABI compat

* Wed Mar 22 2017 Pete Walter <pwalter@fedoraproject.org> - 0.24.6-1
- Update to 0.24.6
- Include previous ABI version for temporary binary compatibility

* Sat Mar 14 2015 Veeti Paananen <veeti.paananen@rojekti.fi> - 0.21.5-1
- Update to 0.21.5
- Backport crash fix

* Wed Aug 06 2014 Christopher Meng <rpm@cicku.me> - 0.21.1-1
- Update to 0.21.1

* Fri Jul 18 2014 Yaakov Selkowitz <yselkowi@redhat.com> - 0.21.0-2
- Fix memory alignment issues on arm, aarch64, ppc64le (#1115905)

* Sat Jun 21 2014 Christopher Meng <rpm@cicku.me> - 0.21.0-1
- Update to 0.21.0

* Fri Jun 06 2014 Karsten Hopp <karsten@redhat.com> 0.20.0-4
- temporarily disable checks on ppc64 and s390x (Bugzilla 1105552)

* Thu Mar 27 2014 Mathieu Bridon <bochecha@fedoraproject.org> - 0.20.0-3
- Fix build requirement on libssh2-devel. (RHBZ#1039433)

* Tue Mar 25 2014 Mathieu Bridon <bochecha@fedoraproject.org> - 0.20.0-2
- Build with the bundled xdiff.
- Disable a failing test. (libgit2#2199)
- Add missing build requirement on libssh2. (RHBZ#1039433)
- Build a thread-safe libgit2.

* Sun Nov 24 2013 Ignacio Casal Quinteiro <icq@gnome.org> - 0.20.0-1
- 0.20.0

* Sat Aug 03 2013 Fedora Release Engineering <rel-eng@lists.fedoraproject.org> - 0.19.0-2
- Rebuilt for https://fedoraproject.org/wiki/Fedora_20_Mass_Rebuild

* Tue Jun 25 2013 Veeti Paananen <veeti.paananen@rojekti.fi> - 0.19.0-1
- 0.19.0

* Wed Jun 19 2013 Dan Horák <dan[at]danny.cz> - 0.18.0-5
- Add htonl() and friends declarations on non-x86 arches
- Rebuilt with fixed libxdiff for big endian arches

* Thu May 30 2013 Veeti Paananen <veeti.paananen@rojekti.fi> - 0.18.0-4
- Update the http-parser patch
- Skip tests that require network connectivity

* Thu May 30 2013 Tom Callaway <spot@fedoraproject.org> - 0.18.0-3
- use system libxdiff instead of bundled copy

* Fri May 24 2013 Veeti Paananen <veeti.paananen@rojekti.fi> - 0.18.0-2
- Remove unnecessary CMake build flags
- Fix the pkgconfig file

* Thu May 02 2013 Veeti Paananen <veeti.paananen@rojekti.fi> - 0.18.0-1
- Update to version 0.18.0
- Unbundle the http-parser library

* Fri Oct 19 2012 Veeti Paananen <veeti.paananen@rojekti.fi> - 0.17.0-2
- Use make for building and installation
- Specify minimum CMake version
- Remove useless OpenSSL build dependency
- Move development documentation to the -devel package
- Add code examples to the -devel package

* Thu Oct 18 2012 Veeti Paananen <veeti.paananen@rojekti.fi> - 0.17.0-1
- Initial package.
