# iiSocietyHelper: Windows 빌드 및 실행

컨테이너 상태·발행·관찰·동기화 세션과 직접 파일 다운로드를 결합하는 Qt 기반 SDK이다.

검증 기록 갱신: `2026-10-20T02:45:37+09:00`. 변경 전 소스 기준 commit: `f549ae3c9ba1a38b0e43ecd70611209bafd5284e`. 아래 결과는 이번 변경을 포함한 작업 트리의 검증이다.

## 현재 검증 상태

Release 빌드·실행 검사·설치를 완료하였다. 등록 검사 10개 중 10개 통과, 0개 실패, 0개 skip이다.

## 이 프로젝트의 의존성과 실행 범위

선행 workspace SDK는 다음과 같다. 각각의 Windows 지침에 따라 실행 검증과 설치를 완료한 뒤 진행한다.

- [iiAcountManager](../../../SDK/iiAcountManager/docs/WindowsBuildAndRun.md)
- [iiFileProvider](../../../SDK/iiFileProvider/docs/WindowsBuildAndRun.md)
- [iiSharedCanvas](../../../SDK/iiSharedCanvas/docs/WindowsBuildAndRun.md)
- [iiSocietyContainer](../../../SDK/iiSocietyContainer/docs/WindowsBuildAndRun.md)
- [iiSocietyMonitor](../../../SDK/iiSocietyMonitor/docs/WindowsBuildAndRun.md)
- [iiSocietySync](../../../SDK/iiSocietySync/docs/WindowsBuildAndRun.md)

- iiFileProvider, iiSocietyContainer, iiSharedCanvas Core 0.31.1 EXACT, iiAcountManager, iiSocietyMonitor 및 iiSocietySync가 필요하다.
- Qt Core/Sql/Network/Concurrent/Test 6.8.3과 OpenSSL Crypto를 사용한다.

프로젝트별 Windows 조건은 다음과 같다.

- CLI target `ii-society-helper`와 다중 프로세스 관찰·세션 테스트를 함께 빌드한다.
- 실제 호스트 파일 다운로드에는 유효한 인증된 host session이 필요하다. 기존 iiSocietyClient의 별도 저장소는 현행 프로젝트 범위에 포함하지 않는다.

## 환경 준비

DNS-SD는 Windows용 vcpkg `mdnsresponder:x64-windows`를 사용한다. workspace SDK 루트의 `Windows/vcpkg-overlays`에는 MFC 기능을 사용하지 않는 `.rc`의 header를 Windows SDK의 `winres.h`로 바꾸는 overlay를 제공한다. 이 구성은 별도 MFC 설치 없이 빌드한다.

```powershell
& "$workspace/Tools/vcpkg/vcpkg.exe" install 'mdnsresponder:x64-windows' "--overlay-ports=$workspace/SDK/Windows/vcpkg-overlays"
if ($LASTEXITCODE -ne 0) { throw 'DNS-SD 개발 의존성 설치 실패' }
```

PowerShell에서 이 프로젝트 루트로 이동한 뒤 순서대로 실행한다. workspace 배치는 `Workspace/SDK/<이름>`을 전제로 한다.

검증 도구는 MSVC 19.51 x64, Windows SDK 10.0.28000, CMake 4.4.4, Ninja 1.13.2이다. 개발 shell은 빌드 재현을 위해 Python 3.14.8을 선택하며 시스템 기본 Python 3.15.0과 구분한다.

```powershell
$ErrorActionPreference = 'Stop'
$project = (Get-Location).Path
$workspace = (Resolve-Path -LiteralPath '../..').Path
. (Join-Path $workspace 'SDK/Windows/Enter-DevEnvironment.ps1')
Set-Location -LiteralPath $project
$python = (Join-Path $workspace 'Tools/Python314/tools/python.exe').Replace('\', '/')
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source.Replace('\', '/')
$toolchain = (Join-Path $workspace 'Tools/vcpkg/scripts/buildsystems/vcpkg.cmake').Replace('\', '/')
$msvcInclude = (Join-Path $workspace 'SDK/Windows/WindowsMsvcPrefix.cmake').Replace('\', '/')
$prefix = (Join-Path $env:USERPROFILE '.local/SDK/iiSocietyHelper').Replace('\', '/')
```

`Enter-DevEnvironment.ps1`은 Qt MSVC kit, vcpkg, 설치된 SDK의 검색 경로와 UTF-8 설정을 준비한다. `CMAKE_PREFIX_PATH`의 디렉터리 존재만으로 의존성 검증을 대신하지 않는다. 선행 SDK의 실행 검사와 설치를 먼저 완료해야 한다.

## 구성과 빌드

다음은 선행 의존성을 복구한 뒤 실행할 절차이다. 현재 이 프로젝트에서 성공한 명령이라는 의미는 아니다. 버전 EXACT 요구도 함께 확인한다.

```powershell
$configure = @(
    '-S', '.', '-B', 'build', '-G', 'Ninja'
    '-DCMAKE_BUILD_TYPE=Release'
    "-DCMAKE_C_COMPILER=$compiler"
    "-DCMAKE_CXX_COMPILER=$compiler"
    "-DCMAKE_INSTALL_PREFIX=$prefix"
    "-DCMAKE_PREFIX_PATH=$env:CMAKE_PREFIX_PATH"
    "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
    '-DVCPKG_TARGET_TRIPLET=x64-windows'
    "-DCMAKE_PROJECT_INCLUDE=$msvcInclude"
    "-DPython3_EXECUTABLE=$python"
    "-DPython_EXECUTABLE=$python"
    '-DQt6_DIR=C:/Qt/6.8.3/msvc2022_64/lib/cmake/Qt6'
    '-DBUILD_TESTING=ON'
)
& cmake @configure
if ($LASTEXITCODE -ne 0) { throw 'CMake 구성 실패' }
$cache = Get-Content -LiteralPath 'build/CMakeCache.txt' -Raw
if ($cache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\r?$') {
    throw 'Release 구성이 유실되었음. 같은 구성 인수에 --fresh를 추가하여 다시 구성할 것'
}
& $python (Join-Path $workspace 'SDK/Windows/Configure-WindowsNinja.py') (Join-Path $project 'build')
if ($LASTEXITCODE -ne 0) { throw 'Ninja MSVC 의존성 설정 실패' }
cmake --build build --config Release --parallel 4
if ($LASTEXITCODE -ne 0) { throw '빌드 실패' }
```

모든 생성 파일은 이 프로젝트의 `build/` 아래에 둔다. 컴파일러는 위에서 확인한 절대 경로를 사용한다. 기존 캐시가 다른 compiler 또는 generator를 사용한다면 같은 구성 인수에 `--fresh`를 추가하여 다시 구성한다. MSVC 키트와 MinGW 또는 Debug/Release 라이브러리를 혼합하지 않는다.

`WindowsMsvcPrefix.cmake`와 `Configure-WindowsNinja.py`는 MSVC include 출력 언어를 Ninja에 맞추고 UTF-8 실행 manifest를 적용한다. 빌드 병렬도는 4로 제한하며 실행 검사는 직렬로 수행한다.

## 실행과 확인

실제 CLI 간 발견·관찰·종료, client session·파일 delivery와 설치 소비자를 검증하도록 구성한다.

```powershell
$dllDirectories = @(Get-ChildItem -LiteralPath build -Recurse -Filter '*.dll' -File |
    Select-Object -ExpandProperty DirectoryName | Sort-Object -Unique)
$env:PATH = (@($dllDirectories) + @($env:PATH)) -join ';'
$env:QSG_RENDER_LOOP = 'basic'
ctest --test-dir build -C Release --output-on-failure --parallel 1 --no-tests=error
if ($LASTEXITCODE -ne 0) { throw '전체 실행 검사 실패' }
```

현재 선행 의존성이 해결되지 않아 이 빌드의 CTest 목록은 생성되지 않았다. 성공적으로 구성한 뒤 `ctest --test-dir build -N`으로 테스트 등록을 확인한다. 테스트 0개는 실행 성공으로 처리하지 않는다.

## 설치와 배포 범위

전체 테스트가 통과한 구성만 설치한다. skip된 선택 기능은 지원·실행 범위를 별도 기록한다.

```powershell
cmake --install build --config Release --prefix "$prefix"
if ($LASTEXITCODE -ne 0) { throw '설치 실패' }
```

설치 파일은 `build/install_manifest.txt`와 실제 prefix의 파일을 대조한다. 소비자 실행 시 prefix의 `bin`과 필요한 vcpkg DLL을 검색할 수 있어야 한다.

## 검증 근거

- 빌드 정의: [CMakeLists.txt](../CMakeLists.txt)와 프로젝트 테스트 등록이다.
- 이번 실행 결과: [Windows SDK 전체 검증 기록](https://github.com/iisacc-Justmoong/SDK/blob/main/Windows/Verification-20261020.md)이다.
- 프로젝트별 실행 로그와 JUnit은 같은 검증 디렉터리에 보관한다. 로그의 종료 코드, 실패·skip 목록과 실제 실행 범위를 함께 확인한다.

전체 SDK의 실행·설치와 커밋 조건은 [공통 Windows 검증 도구](https://github.com/iisacc-Justmoong/SDK/blob/main/Windows/README.md)에서 재현한다. 원본 JSON·JUnit·상세 로그는 로컬 `Workspace/build/maintenance/20261019-221920-sdk-recovery/final/`에 보관하며 Git에는 포함하지 않는다. 이 프로젝트의 성공은 제품·다른 플랫폼·별도 계정 및 물리 장치의 실연동을 대신하지 않는다.
