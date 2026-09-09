# iiSocietyHelper

C++20·Qt 6.8.3 기반 iisacc 앱 공통 라이브러리이다. 버전 0.7.0은 **C++ 객체 전송**, iisacc 계정 객체 참조, Society 원본 파일 시스템 접근, 같은 기기의 앱 관측, 영속 데이터 전달을 제공한다. 모든 참여자는 자기 생존 신호를 기록하면서 다른 참여자의 신호를 읽는다. Society 앱이나 별도 중앙 서버가 켜져 있을 필요가 없다.

## 앱에서 사용하기

```cmake
find_package(iiSocietyHelper 0.7.0 CONFIG REQUIRED)
target_link_libraries(my_application PRIVATE iiSocietyHelper::iiSocietyHelper)
```

```cpp
#include <iiSocietyHelper.h>
#include <QCoreApplication>
#include <QDebug>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    iiSocietyHelper::Helper helper;
    QObject::connect(&helper, &iiSocietyHelper::Helper::peerAppeared,
                     &app, [](const iiSocietyHelper::Peer &peer) {
        qInfo() << peer.application.id << peer.instanceId;
    });
    if (!helper.start({"com.iisacc.example", "Example", "1.0.0"})) {
        qWarning() << helper.errorString();
        return 1;
    }
    return app.exec();
}
```

Helper는 `QCoreApplication` 생성 후 앱의 이벤트 루프 스레드에서 생성·호출하며, 관측하는 동안 살아 있어야 한다. DLL을 링크하는 것만으로 관측을 시작하지 않는다. 실제 앱 진입점에서 `start()`를 호출한다. 종료 시 `aboutToQuit`, 소멸자 또는 명시적인 `stop()`이 자기 기록을 제거한다. `stop()` 후 다시 시작하면 새 인스턴스 ID를 발급한다. 시작 중복 호출은 기존 관측을 유지하면서 오류를 반환한다. 시그널 처리에서 객체를 없애려면 Qt의 `deleteLater()`를 사용한다.

Society와 Dreamscapes의 LVRS `configureEngine`에서 이 수명을 연결한다. Qt Quick 앱은 `QGuiApplication::applicationStateChanged`를 `setActivity()`에 연결할 수 있다. `Activity`는 Unknown/Foreground/Background이며, 포커스를 잃었다는 이유만으로 죽은 앱으로 처리하지 않는다.

## 계정 객체 참조 (0.6.0)

앱에서 사용하는 `iisacc::accounts::AccountManager`를 `helper.setAccountManager(&accounts)`로 연결한다.
실제 패키지·헤더 이름은 기존 저장소와 같은 `iiAcountManager`이다. Helper는 매니저와 계정을 복사하거나
소유하지 않으며, 로그인은 연결된 매니저가 수행한다. 참조는 관측 `start()` 전부터 사용하고 `stop()` 후에도 유지한다.

```cpp
#include <iiSocietyHelper.h> // AccountManager와 Account의 공개 헤더도 포함한다.

iisacc::accounts::AccountManager accounts;
iiSocietyHelper::Helper helper;
if (!helper.setAccountManager(&accounts)) return;
QObject::connect(&helper, &iiSocietyHelper::Helper::accountChanged, &helper, [&] {
    const auto *user = helper.account();
    if (!user || !user->isPresent()) return;
    const QString userId = user->userId();
    const auto *author = user->authorDetails();
    const QVariantMap allValues = user->toVariantMap();
});
accounts.loginWithPassword(email, password);
// accounts.verificationRequired() 이후 accounts.verifyEmailCode(code)를 호출한다.
```

| API / Qt 속성 | 계약 |
| --- | --- |
| `setAccountManager(AccountManager*)` | 같은 스레드의 매니저를 연결한다. `nullptr`은 연결 해제이다. QML에서도 호출할 수 있다. |
| `accountManager()` / `accountManager` | 연결한 매니저 객체 자체이다. 미연결·파괴 후에는 `nullptr`이다. |
| `account()` / `account` | 매니저의 고정 `Account` 객체 자체이다. 미연결이면 `nullptr`, 연결 후 로그인 전에는 `present == false`이다. |
| `accountManagerChanged()` | 연결·교체·연결 해제·매니저 파괴를 알린다. |
| `accountChanged()` | 참조 교체와 계정·중첩 작성자 데이터 변경을 알린다. 변경된 전체 값이 반영된 뒤 보낸다. |

`Account`의 신원·프로필·멤버십·동의 정보 10개 필드와 `AuthorDetails`의 20개 필드, 타입이 지정된 링크와
식별자 컬렉션을 그대로 읽는다. QML에서는 `societyHelper.account.userId`,
`societyHelper.account.authorDetails.organization`, `societyHelper.accountManager.state`를 사용할 수 있다.
연결이 없을 때에는 `societyHelper.account`를 먼저 검사한다. 계정 변경 알림은 로그인 요청 상태 변경과
구분하며, 로그인 상태는 매니저의 `state`·`stateChanged`를 사용한다.

같은 매니저를 다시 설정하면 중복 알림이 없다. 교체하면 이전 매니저의 알림 연결을 끊고, 매니저 파괴 시
두 참조를 자동으로 비운다. 여러 Helper가 같은 매니저를 참조할 수 있다. Helper 해제·파괴는 매니저의
로그아웃이나 계정 초기화를 수행하지 않는다. `setAccountManager()`는 Helper의 이벤트 루프 스레드에서
호출하며 매니저도 같은 스레드에 유지한다. 다른 스레드는 변경 없이 `false`를 반환한다. 변경 알림 처리 중
다른 참조로 교체되거나 Helper가 파괴되어도 이전 참조의 후속 알림을 보내지 않고 `false`를 반환한다.

이 연결은 같은 프로세스 안의 QObject 참조이다. 계정 데이터·인증 쿠키를 관측 기록이나 전달 큐에 자동으로
저장·방송하지 않으며, 파일 시스템 접근 권한·저장 위치·원격 동기화 정책을 변경하지 않는다.

의존성 방향은 `iiSocietyHelper → iiAcountManager → Qt Core/Network`이다. 기존 Workspace에서 관리하는
계정 SDK 0.2.x의 공개 API를 재사용한다. 계정 모델·로그인 구현의 복제나 새 외부 패키지 도입은 없다.
Helper의 CMake 타깃이 계정 SDK를 공개 링크하고 설치 설정도 `find_dependency`로 찾으므로 소비자는
Helper만 링크하면 된다. 계정 SDK에서 Helper나 Container를 참조하지 않는다.

## Society 파일 시스템 접근 (0.4.0)

`Helper`의 `fileSystem()`은 생성 시 Society 원본을 자동으로 열며 `start()` 이전과 `stop()` 이후에도 사용한다. Helper만 링크하면 iiSocietyContainer 0.9.0과 Qt도 연결된다. 기존 QML 컨텍스트에서는 `societyHelper.fileSystem`으로 사용한다.

```cpp
auto *storage = helper.fileSystem();
const auto directory = storage->ensureDirectory("generation-history", "Example/session-1");
if (directory.isEmpty()) { qWarning() << storage->errorString(); return; }
const auto path = storage->path("generation-history", "Example/session-1/result.json");
if (path.isEmpty()) { qWarning() << storage->errorString(); return; }
QSaveFile file(path); // #include <QSaveFile>
const QByteArray bytes = "{\"status\":\"saved\"}";
if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
    qWarning() << file.errorString();
```

`path()`는 `QFile`, `QDir`, `std::filesystem` 및 외부 엔진이 읽고 쓸 수 있는 실제 절대 경로이다. `url()`은 공백·한글·`#`·`%`를 보존하는 로컬 파일 URL이다. QML 예시는 `societyHelper.fileSystem.url("models", "weights.safetensors")`이다. `sections`는 `{key, name, path}` 목록이며 키는 `asset-library`, `deleted`, `files`, `forked`, `generation-history`, `models`, `published`, `thinking-space`이다.

- `open()`의 우선순위는 명시 원본 경로, `SOCIETY_CONTAINER_PATH`, Society 공통 설정이다. iOS는 기존 Society App Group 원본을 사용한다. Helper는 컨테이너를 새로 만들거나 전역 기본 선택을 바꾸지 않는다.
- 성공한 선택은 경로와 UUID에 고정된다. 기본 저장소가 바뀌어도 작업 중인 대상은 유지한다. `refresh()`는 현재 선택 방식을 다시 적용하고, `open(path)`는 명시 경로, 인자 없는 `open()`은 기본 선택으로 돌아간다. 선택 실패 시 이전 저장소로 쓰지 않는다.
- 최초 저장소가 없으면 빈 경로와 `fileSystem.errorString`을 반환하고 다음 경로 요청에서 다시 시도한다. 열었던 드라이브가 사라지거나 교체되면 명시적 재선택 전에는 새 UUID로 전환하지 않는다.
- `available`, `rootPath`, `containerId`, `sections`는 조회 시 유효성을 확인한다. `storageChanged`는 선택·재선택 및 접근 중 감지한 무효화를 알린다. 상시 디스크 감시는 없으므로 화면 복귀·새로 고침 시 `refresh()`를 호출한다. 파일 오류는 관측·전달의 `Helper.errorString`과 별도이며 관측을 중단하지 않는다.
- `path(key)`는 영역 디렉터리이다. 상대 경로는 마지막 파일 이름만 없어도 반환하며 부모는 `ensureDirectory()`로 먼저 준비한다. 숨김 항목은 지원한다. 절대 경로, `..`, `.`, 빈 중간 요소, 역슬래시, 콜론, NUL, 심볼릭 링크·junction은 거부한다. 경로 조회는 파일을 만들지 않는다.

앱들은 같은 원본 바이트를 공유한다. Finder·iOS 파일 앱에는 기존대로 `Files/`만 공개하고 Helper 앱은 8개 영역을 사용한다. 경로 반환은 파일 잠금이나 I/O 성공을 보장하지 않는다. 실제 오류는 파일 API에서 처리하며 동시 편집은 앱의 잠금·충돌 정책을 적용한다. 반환 뒤 파일 시스템이 바뀔 수 있으므로 작업 직전에 경로를 다시 구한다. 원격 동기화와 OS sandbox 권한 부여는 별도이다.

iOS 소비 앱은 기존 `iiSocietyContainer_configure_ios_client()` 구성과 동일 App Group 서명이 필요하다. macOS sandbox 앱도 OS에서 허용한 원본을 사용해야 한다. Android·WebAssembly의 개인 앱 디렉터리는 공용 저장소가 아니며 호스트가 실제로 공유한 원본을 명시해야 한다. Android 앱 간 기본 공유 저장소·IPC는 제공하지 않는다. [Apple App Group](https://developer.apple.com/documentation/xcode/configuring-app-groups)의 공유 컨테이너 계약을 따른다.

Helper → Container → Qt Core 방향으로 의존한다. 발견·UUID·영역·경로 검사는 같은 Workspace에서 관리하는 AGPL-3.0-only iiSocietyContainer를 재사용하고 추가 서버·드라이버·외부 라이브러리는 도입하지 않는다. 일반 I/O는 기존 [QFile](https://doc.qt.io/qt-6.8/qfile.html), [QSaveFile](https://doc.qt.io/qt-6.8/qsavefile.html)을 사용하며 Qt 라이선스는 설치본을 따른다. 구현은 루트의 `FileSystem.cpp`, 공개 API는 `iiSocietyHelper.h`에 있다.

`iiSocietyHelper.filesystem` 및 설치 소비자는 서로 다른 프로세스의 표준 C++ 쓰기 → Qt 읽기·수정 → 표준 C++ 재읽기, 8개 영역, 이름 변경·삭제, URL, 잘못된 경로, 늦은 저장소 준비, 선택 고정·재선택, UUID 교체를 검사한다. 모든 파일은 `build/`의 임시 컨테이너에 한정한다.

`build/install/bin/ii-society-helper --filesystem`은 관측·데이터베이스를 시작하지 않고 현재 원본의 경로·UUID·8개 영역을 JSON으로 출력한다. 기본 저장소가 없거나 잘못되면 오류와 종료 코드 1을 반환한다. 명시 원본은 `SOCIETY_CONTAINER_PATH`로 지정한다. QML 표현식과 이 진단 명령의 읽기 전용 동작도 설치 소비자에서 검증한다.

## 관측 계약

- `ApplicationInfo`: 앱을 구분하는 고정 reverse-DNS ID, 표시 이름, 버전이다.
- `Peer`: 앱 정보, 실행별 UUID, PID, 전경/배경 상태, 시작 시각, 마지막 신호를 확인한 시각이다. PID를 식별 키로 사용하지 않아 PID 재사용과 같은 앱의 여러 실행을 구분한다.
- `peers()`와 QML용 `observedApplications`에는 자기 자신을 제외한 현재 관측 가능한 인스턴스가 들어간다. UUID 순으로 정렬한다.
- `peerAppeared`, `peerUpdated`, `peerDisappeared`와 `peersChanged`를 제공한다. 생존 신호만 갱신되면 메타데이터 변경 이벤트를 반복하지 않는다.
- `DepartureReason::Withdrawn`은 기록이 사라지거나 유효하지 않게 되었음을, `TimedOut`은 신호가 끊겼음을 뜻한다. 앱의 종료 원인이나 OS 프로세스 사망을 단정하는 상태가 아니다.
- 저장된 파일을 처음 읽은 것만으로 실행 중이라고 판정하지 않는다. 그 뒤 heartbeat 순번이 증가해야 발견 이벤트를 낸다. 강제 종료·재부팅 후 남은 기록은 발견되지 않으며, 다른 Helper의 파일을 삭제하지 않는다.
- 기본 생존 신호 간격은 1초, 관측 만료는 5초이다. 발견에는 보통 한두 번의 갱신이 필요하다. OS 스케줄링에 따라 지연될 수 있다. `ObservationOptions`로 간격을 바꾸는 참여자는 서로의 주기를 수용하는 timeout을 사용해야 한다.
  상호 발견·중단 후 복구 통합 검사도 실제 기본 만료 시간인 5초를 사용한다. 외장 볼륨의 파일 flush 지연을 정상 실행 중인 참여자의 만료로 오인하지 않으며, 복구 검사는 실제로 5.5초 동안 이벤트 루프를 중단해 만료와 새 heartbeat 재발견을 모두 확인한다.
  별도 프로세스 검사 역시 5초 만료를 사용하며, 재시작 프로세스는 발견을 확인한 뒤 정상 종료 이벤트까지 관측할 수 있도록 10초 동안 실행한다.
- 만료는 관측자 내부의 단조 시계를 사용한다. 기록의 날짜나 파일 수정 시각은 생존 증거가 아니다. 관측자 자체가 timeout 이상 중단되었다가 돌아오면 다시 새로운 신호를 확인한다.
- 실행 중 디렉터리 접근이나 기록에 실패하면 `running=false`로 전환하고 `errorOccurred`/`errorString`을 제공한다. 원인을 해결한 뒤 `start()`로 재시작할 수 있다.

이는 협력하는 앱들의 존재 확인이다. 설치된 모든 앱 검색, 화면·문서 내용 수집, 프로세스 제어, 앱 간 명령 전달, 기기 간 발견·동기화는 포함하지 않는다. 앱 ID는 참여자가 선언하므로 서명 검증이나 인증 수단으로 사용하지 않는다.

## C++ 객체 전송 (0.7.0)

`Helper::sendObject(topic, object)`가 객체의 현재 값을 동기적으로 직렬화하여 기존 영속 전달 큐에 넣는다.
성공 시 메시지 UUID, 실패 시 빈 문자열과 `errorString()`을 반환한다. `dataQueued(id)`는 실제 큐 삽입
성공 후 한 번 발생한다. 전송 성공 후 원본 값 수정·QObject 삭제·송신 앱 종료는 큐에 저장한 값을 바꾸지 않는다.
먼저 `start()`로 송신 앱·관측 디렉터리를 구성한다. 호출 중 getter나 직렬화 연산자가 Helper를 중단·재시작하면
다른 송신 인스턴스 이름으로 객체를 넣지 않고 실패하며, Helper가 파괴되어도 남은 객체 전송을 수행하지 않는다.

| 전송 대상 | 송신 API | 복원 결과 |
| --- | --- | --- |
| `QObject*` / `const QObject*` / `const QObject&` | `sendObject(topic, object)` | `ObjectSnapshot { QString className; QVariantMap properties; }` |
| Qt 값·등록한 C++ struct/class | `sendObject(topic, value)` | 원래 Qt 메타타입을 보존한 `QVariant`, `value<T>()`로 C++ 값 복원 |
| 이미 보유한 `QVariant` | `sendObject(topic, variant)` | 담겨 있던 값의 타입과 내용 |
| 기존 JSON 값 묶음 | `sendData(topic, map)` | 기존 `QVariantMap` 계약 |

QObject는 `Q_INVOKABLE QVariantMap toVariantMap() const`가 있으면 그 명시적 저장 계약을 사용한다.
그렇지 않으면 상속된 사용자 속성을 포함한 읽기 가능한 `STORED` Q_PROPERTY를 읽는다. QObject 기본
`objectName`, 동적 속성, `STORED false` 속성은 자동 수집하지 않는다. 중첩 QObject 속성은 중첩
`ObjectSnapshot`이 되고 null 참조는 null 값이 된다. 순환 참조는 거절한다. 객체는 자기 스레드에서 읽고,
getter는 유효한 값·참조를 반환해야 한다. 원본 QObject와 실행 메서드·소유권·메모리 주소는 전송하지 않는다.
원래 QObject 클래스의 인스턴스가 필요하면 수신 앱이 스냅샷과 해당 클래스의 생성·갱신 API를 사용한다.

계정 모델은 기존 전체 저장 계약을 그대로 사용한다. 로그인 후 10개 계정 필드와 작성자 정보 20개를 담으며,
계정 참조 연결만으로 자동 전송하지 않고 다음 호출에서 명시적으로 전송한다.

```cpp
// helper.start(...)와 로그인 완료 후, 같은 이벤트 루프 스레드에서 호출한다.
helper.setAccountManager(&accounts);
const QString id = helper.sendObject("account.profile", helper.account());

// 수신 측: DeliveryStore 또는 SocietyInbox에서 얻은 해당 메시지이다.
QString error;
const QVariant decoded = iiSocietyHelper::ObjectCodec::decode(message["payload"].toMap(), &error);
if (decoded.metaType() == QMetaType::fromType<iiSocietyHelper::ObjectSnapshot>()) {
    const auto snapshot = decoded.value<iiSocietyHelper::ObjectSnapshot>();
    iisacc::accounts::AccountManager reader;
    if (snapshot.className == "iisacc::accounts::Account")
        reader.readAccount(snapshot.properties);
}
```

이렇게 복원한 계정은 전달받은 프로필 값이다. 로그인 세션과 서버 권한을 발급하지 않는다. QML에서도
`societyHelper.sendObject("account.profile", societyHelper.account)`로 QObject를 직접 전달할 수 있다.

일반 C++ 값 객체는 복사·기본 생성이 가능해야 하며, 송신·수신 앱이 같은 메타타입과 QDataStream 연산자를
공유해야 한다. 다음 선언·연산자를 공통 헤더에 두고 수신 앱에서도 `qRegisterMetaType`을 호출한다.

```cpp
#include <QDataStream>
#include <QMetaType>
#include <QString>

struct GenerationRequest { QString prompt; quint64 seed = 0; };
inline QDataStream& operator<<(QDataStream& out, const GenerationRequest& value) {
    return out << quint32(1) << value.prompt << value.seed;
}
inline QDataStream& operator>>(QDataStream& in, GenerationRequest& value) {
    quint32 version = 0;
    in >> version;
    if (version != 1) { in.setStatus(QDataStream::ReadCorruptData); return in; }
    return in >> value.prompt >> value.seed;
}
Q_DECLARE_METATYPE(GenerationRequest)

// 앱 시작 시, 양쪽 프로세스에서 실행한다.
qRegisterMetaType<GenerationRequest>();
GenerationRequest request{"a forest", 18446744073709551615ULL};
const QString id = helper.sendObject("generation.request", request);

// 수신 앱에서 topic을 확인한 뒤 처리한다.
QString error;
const QVariant value = iiSocietyHelper::ObjectCodec::decode(message["payload"].toMap(), &error);
if (value.metaType() == QMetaType::fromType<GenerationRequest>()) {
    const auto restored = value.value<GenerationRequest>();
}
```

Q_GADGET을 포함한 사용자 값 타입도 동일한 스트림 연산자 계약을 따른다. QObject 속성에 담긴 사용자
값 타입도 수신 측에서 등록해야 한다. 메타타입 등록만으로 임의 C++
멤버를 자동 직렬화하지 않는다. 사용자 연산자가 필드·스키마 버전·내부 컬렉션 제한을 정의한다.
`ObjectCodec::encode()` / `decode()`는 전송과 별도로 사용할 수 있다. 등록되지 않은 수신 타입, 지원하지
않는 스트림 연산자, QVariant 안의 원시·QObject 스마트 포인터, 잘린 데이터, 후행 바이트, 잘못된 Base64와
지원하지 않는 프로토콜 버전은 명시적으로 거절한다. 실패한 객체는 부분 메시지로 큐에 남지 않는다.

payload는 `format: "iisacc.qt-object"`, `version: 1`, `streamVersion: 22`, `typeName`, `data`의 다섯 필드이다.
`data`는 Qt 6.8 QDataStream(BigEndian, DoublePrecision) 바이트의 Base64이다. 이 형식으로 QByteArray,
64비트 부호·무부호 정수, QDateTime, QUrl과 사용자 값 타입을 보존한다. 원시 스트림 쓰기는 48 KiB로 제한하고,
**메타데이터·Base64를 포함한 최종 payload는 기존 64 KiB 한도**를 따른다. 검사하는 QObject 스냅샷과
QVariantMap/List/Hash에는 중첩 깊이 16·노드 4096 한도를 적용한다. 큰 객체는 저장소의 파일과 참조를 사용한다.

기존 outbox/inbox·데몬 수신·재시도·확인 위치를 그대로 사용하며 DB 스키마를 바꾸지 않는다. 데몬은
객체 payload를 운반하고 수신 앱이 `ObjectCodec::decode()`로 복원한다. 현재 설치된 Qt Core의
QMetaObject·QMetaType·QDataStream을 재사용하여 새 외부 라이브러리·직렬화 엔진을 추가하지 않았다.
별도 프로세스 검사와 설치 소비자가 같은 API와 공개 헤더로 송신 종료 이후 복원을 확인한다.

## Society 데몬으로 데이터 전달

`Helper::sendData(topic, payload)`는 최대 64 KiB의 JSON 객체를 영속 발신 큐에 넣고 메시지 UUID를 반환한다. 빈 문자열이면 기록에 실패했으므로 `errorString()`을 확인한다. 반환 성공은 **디스크에 대기 데이터가 저장되었음**을 뜻하며 데몬 수신 완료와 구분한다. 큰 모델·이미지 자체 대신 Society에 저장된 에셋의 참조를 전달한다.

```cpp
const QString messageId = helper.sendData("generation.queued", {
    {"jobId", jobId}, {"modelPath", modelPath}
});
```

Helper는 `helper.started`, `helper.activity`, `helper.peerAppeared`, `helper.peerUpdated`, `helper.peerDisappeared`, `helper.stopped` 이벤트도 자동으로 기록한다. 생존 신호마다 이력을 추가하지 않는다. 데몬이 꺼져 있어도 발신 큐에 남고, Helper가 종료되어도 사라지지 않는다.

여러 앱의 첫 실행이 겹쳐도 초기 스키마와 WAL 설정이 충돌하지 않도록 초기화에만 프로세스 간 잠금을 사용한다. 일반 발신·수신은 SQLite 트랜잭션을 사용한다.

공유 위치의 `delivery/delivery.sqlite`에는 outbox, inbox, 소비자 확인 위치, 마지막 데몬 스냅샷이 있다. `DeliveryStore::receivePending()`은 단일 SQLite 트랜잭션에서 발신 데이터를 수신함으로 옮긴다. WAL·FULL 동기화와 메시지 ID 고유 제약을 사용하며 트랜잭션이 실패하면 발신 데이터를 유지한다. 프로세스가 중단된 뒤 다시 처리해도 같은 ID를 중복 저장하지 않는다. Qt SQL 드라이버와 파일 시스템이 반환하는 오류를 호출자에게 전달한다.

Society 제품의 독립 실행 파일 `SocietyDaemon`이 이 작업을 수행한다. 앱 본체는 `SocietyInbox`를 통해 순번대로 데이터를 받고, `dataReceived(QVariantMap)`·최근 100개 메시지·페이지 읽기 API를 제공한다. `DeliveryStore::readAfter(sequence, limit)`로 데몬이 수신한 과거 데이터도 읽을 수 있다. 데몬의 마지막 스냅샷은 기록된 시점의 상태이며 현재 생존의 증명이 아니다.

읽기는 메시지를 지우지 않는다. 소비자가 처리를 끝낸 뒤 `acknowledge(consumerId, sequence)`를 호출해야 재실행 시 그 이후부터 시작한다. 확인 위치는 뒤로 가지 않으며 아직 수신되지 않은 순번을 확인할 수 없다. 확인 전 재실행은 같은 메시지를 다시 전달할 수 있으므로 소비자는 메시지 ID로 중복 처리를 피한다. 보존 기간이 아직 정의되지 않아 수신 기록을 자동 삭제하지 않는다.

iOS 데이터도 캐시 정리에 의해 없어지지 않도록 0.3.0부터 App Group의 Application Support 아래에 둔다. 0.2.0의 Caches에는 영속 전달 데이터가 없었으므로 관측 기록을 이전하지 않고 새로 발견한다. iOS에서는 상시 별도 데몬을 실행할 수 없으므로 Society가 실행되는 동안 같은 수신 서비스를 가동하고, 앱이 중단된 동안에는 공유 발신 큐가 데이터를 보관한다. iOS 실기기 검증은 별도이다.

추가 의존성 근거: [Qt SQL 드라이버](https://doc.qt.io/qt-6.8/sql-driver.html), [SQLite WAL](https://sqlite.org/wal.html), [SQLite 동기화 설정](https://sqlite.org/pragma.html#pragma_synchronous). Qt와 SQLite 구성 요소는 기존 배포본의 라이선스를 따른다.

## 저장 위치와 플랫폼

데스크톱 기본 위치는 `QStandardPaths::GenericDataLocation/iisacc/Society/Helpers/v1`이다. macOS 일반 앱에서는 `~/Library/Application Support/iisacc/Society/Helpers/v1`이다. 동일한 사용자와 공유 위치를 쓰는 참여자끼리 관측한다. 관측 상태와 발신·수신 데이터는 Society 드라이브의 콘텐츠 영역에 넣지 않는다.

iOS 및 App Group으로 묶인 Apple 앱은 Info.plist의 `SocietyAppGroup`과 해당 App Group entitlement를 사용한다. 경로는 그룹 컨테이너의 `Library/Application Support/iiSocietyHelper/v1`이며 앱마다 별도 개인 컨테이너로 대체하지 않는다. 기존 iiSocietyContainer의 iOS 앱 패키징 함수가 설정하는 같은 `group.com.iisacc.society` 계약을 재사용한다. 구성되지 않은 iOS 앱은 시작 오류를 반환한다. macOS sandbox 앱도 같은 App Group 구성이 필요하다.

iOS의 백그라운드 앱은 OS에 의해 중단될 수 있다. 중단된 앱은 신호를 갱신할 수 없어 관측에서 만료되고, 실행을 재개하면 다시 발견된다. 상시 백그라운드 실행이나 다른 앱 깨우기를 보장하지 않는다. 두 앱이 동시에 스케줄링되어야 양쪽의 관측이 진행된다. Apple 구현은 Foundation API를 사용한다. 현재 호스트에서는 App Group 경로 코드의 컴파일을 검증하며, 서명된 iOS 앱 두 개의 기기 실행은 별도 검증이 필요하다.

0.3.1부터 iOS는 정적 라이브러리로 빌드한다. 정적 Qt 앱에서는 `qt_import_plugins(AppTarget INCLUDE Qt6::QSQLiteDriverPlugin)`으로 SQLite 드라이버를 포함한다. 공유 전달 디렉터리와 기존 DB/WAL/SHM은 `NSFileProtectionCompleteUntilFirstUserAuthentication`으로 맞추고 새 파일은 디렉터리의 보호를 상속한다. 재부팅 후 첫 잠금 해제 전에는 시작 실패를 처리하고 나중에 다시 시작해야 한다. 호스트의 `ios_directory_syntax`는 이 iOS 분기의 Foundation API 타입을 검사한다. 앱은 중단 시 `stop()`으로 연결을 해제하고 복귀 시 `start()`로 다시 연결할 수 있다. Society 본체는 `SocietyRuntime`으로 이 흐름과 실패 재시도를 관리한다.

Android와 WebAssembly는 개인 앱 저장 공간을 공용 위치로 오인하지 않도록 기본 시작을 거부한다. 그 플랫폼에서 여러 앱에 접근 가능한 호스트 통합이 제공되면 명시적인 디렉터리를 사용할 수 있다. 기본 Android 앱 간 IPC는 이 버전에 포함하지 않는다. 데스크톱 구현은 Qt Core API를 사용하며 현재 실행 검증 플랫폼은 macOS이다.

테스트나 격리된 앱 묶음에는 `ObservationOptions.directory` 또는 `SOCIETY_HELPER_DIRECTORY`로 동일한 **기기 로컬 절대 경로**를 지정한다. 우선순위는 명시적 옵션, 환경변수, 플랫폼 기본 위치 순이다. 클라우드 동기화 폴더나 네트워크 볼륨을 사용하지 않는다. 직접 지정한 경로는 호출자가 접근 범위를 관리한다. 새 최종 디렉터리와 기록은 소유자 전용으로 만들고, 기존 디렉터리 권한은 바꾸지 않는다. 잘못된·큰·지원하지 않는 버전의 기록과 심볼릭 링크는 무시한다.

## 구현과 의존성

기존 Qt Core의 `QSaveFile`로 각 실행의 `<UUID>.json`을 원자적으로 기록하고, `QFileSystemWatcher`와 타이머 폴링을 함께 사용한다. 감시 통지가 합쳐지거나 누락되어도 주기적으로 다시 읽는다. 데이터 전달에는 기존 Qt 배포본의 Qt Sql·QSQLITE 드라이버를 추가로 연결한다. 별도 메시지 브로커나 외부 서버 패키지는 설치하지 않는다. Qt 6.8.3의 유지 중인 SQL API와 SQLite 트랜잭션을 사용하여 직접 만든 파일 저널의 복구 부담을 줄인다. Qt의 사용·재배포는 해당 설치본의 라이선스를 따른다.

공개 헤더는 루트의 `iiSocietyHelper.h`이며 구현은 같은 루트의 `Helper.cpp`, `ObjectCodec.cpp`, `FileSystem.cpp`, `iiSocietyHelper.cpp`에 둔다. Apple 경로 해석만 `platform/apple/ObservationDirectory.mm`에 있다. 이전 `helloWorld()` 심볼은 기존 소비자 호환성을 위해 유지한다.

근거: [Qt 공유 저장 위치](https://doc.qt.io/qt-6.8/qstandardpaths.html), [QSaveFile](https://doc.qt.io/qt-6.8/qsavefile.html), [QFileSystemWatcher](https://doc.qt.io/qt-6.8/qfilesystemwatcher.html), [단조 시계](https://doc.qt.io/qt-6.8/qelapsedtimer.html), [Apple App Groups](https://developer.apple.com/documentation/xcode/configuring-app-groups), [iOS 백그라운드 실행](https://developer.apple.com/documentation/xcode/configuring-background-execution-modes).

## 빌드·검증·설치

CMake 3.24 이상, C++20, iiSocietyContainer **0.9.0** 이상, iiAcountManager **0.2.x**, Qt **6.8.3** Core·Sql·Network와 QSQLITE 드라이버가 필요하다. 계정 SDK 자체를 빌드하려면 CMake 3.31 이상이 필요하다. 테스트에는 같은 버전의 Qt Test·Qml, Apple 빌드에는 Foundation과 Objective-C++ 컴파일러가 필요하다. 모든 산출물은 `build/`에 둔다.

```sh
CMAKE_PREFIX_PATH="/Volumes/Storage/Workspace/SDK/iiSocietyContainer/build/install;/Volumes/Storage/Workspace/SDK/iiAcountManager/build/install" \
  INSTALL_PREFIX="$PWD/build/install" ./install.sh
```

이 스크립트는 빌드·CTest·설치 후 `build/consumer/build/`에서 설치된 공개 헤더와 라이브러리만 사용하는 독립 소비자의 실제 관측을 검증한다. `INSTALL_PREFIX`를 생략하면 기존 SDK 기본 설치 위치인 `$HOME/.local/SDK/iiSocietyHelper`를 사용한다. 이 작업에서는 Workspace의 `build/install` 설치본을 검증한다.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH=/Volumes/Storage/Qt/6.8.3/macos \
  -DiiSocietyContainer_DIR=/Volumes/Storage/Workspace/SDK/iiSocietyContainer/build/install/lib/cmake/iiSocietyContainer \
  -DiiAcountManager_DIR=/Volumes/Storage/Workspace/SDK/iiAcountManager/build/install/lib/cmake/iiAcountManager \
  -DCMAKE_INSTALL_PREFIX="$PWD/build/install" -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
cmake --install build
```

테스트는 세 Helper의 상호 발견, 같은 앱 복수 인스턴스, 상태 변경, 정상 종료, 강제 종료 후 남은 기록, 재시작, 프로세스 중단·복귀, 관측자 이벤트 루프 중단·복귀, 잘못된 설정, 디렉터리 경계와 격리를 검사한다. 별도 프로세스 테스트는 실제로 세 실행 파일을 가동한다. `iiSocietyHelper.account`와 `iiSocietyHelper.installed_account`는 실제 계정 SDK를 연결하여 객체 동일성·전체 모델 접근·갱신·초기화·교체·소멸·재진입·스레드 경계·QML 참조·관측 데이터 분리를 검사한다.

2026-09-09 0.6.0 계정 참조 검증에서 Helper Release 빌드와 CTest **7/7**, `build/install` 설치본만 링크한
`build/account-reference/consumer`의 CTest **5/5**가 통과했다. 계정 참조 검사는 각 실행에서 9개 사례를
검증했다(초기화·정리 포함 11개 통과). 소비자의 CMake는 Helper만 찾고 링크하며, iiAcountManager 0.2.0을
`SDK/iiAcountManager/build/install`에서 의존성으로 해석했다. 실행 시 `DYLD_LIBRARY_PATH`,
`DYLD_FRAMEWORK_PATH`, `DYLD_FALLBACK_LIBRARY_PATH`를 제거했다. 결과 XML은
`build/account-reference-tests.xml`과 `build/account-reference/consumer/account-reference-installed-tests.xml`이다.
이번 참조 검증에는 실제 로그인 요청·운영 배포·Android/iOS 기기 실행을 포함하지 않는다.

2026-09-09 0.7.0 객체 전송 검증에서 Helper 빌드와 CTest **8/8**, `build/install`의 공개 헤더와
라이브러리로 새로 빌드한 `build/object-transfer/consumer`의 CTest **6/6**이 통과했다.
객체 전송 검사는 각 실행에서 12개 사례를 검증했다(초기화·정리 포함 14개 통과).
C++ 값 타입과 64비트 정수·바이너리·날짜·URL의 왕복, QObject 속성과 계정 전체 모델의 스냅샷,
잘못된 형식·크기·순환·스레드의 거부, getter 실행 중 객체 소멸·Helper 재시작, QML 호출을 검사했다.
서로 다른 프로세스로 송신자가 종료된 뒤에도 수신자가 영속 큐에서 객체를 복원하고, 재실행 시 같은
메시지 값을 읽는 것을 확인했다. 소비자는 iiAcountManager 0.2.0을 Workspace의 설치본에서 해석했고,
실행 시 위의 세 `DYLD_*` 변수를 제거했다. 결과 XML은 `build/object-transfer-tests.xml`과
`build/object-transfer/consumer/object-transfer-installed-tests.xml`이다. 이번 검증 범위는 macOS 로컬
프로세스 간 전달이며, 운영 배포·실제 로그인·Android/iOS 기기 실행은 포함하지 않는다.

진단 실행 파일도 설치한다. 두 터미널에서 동일한 위치로 실행하면 양쪽에서 JSON Lines 형태의 발견·변경·이탈 이벤트를 확인할 수 있다. 진단 실행 파일도 하나의 참여자이다.

```sh
build/install/bin/ii-society-helper --directory "$PWD/build/observe" \
  --application-id com.iisacc.example.one --exit-after-ms 15000
build/install/bin/ii-society-helper --directory "$PWD/build/observe" \
  --application-id com.iisacc.example.two --exit-after-ms 15000
```

SDK의 `iisacc.society.helper` Qt 로그에는 관측 시작과 peer 발견·이탈·오류가 기록된다. UI 없이 실제 앱의 양방향 관측 여부를 확인할 수 있다.

## License

SPDX-License-Identifier: AGPL-3.0-only

iiSocietyHelper의 자체 작성 코드와 문서는 GNU Affero General Public License v3.0 전용이다. 전체 조건은 [LICENSE](LICENSE)를 따른다. Qt와 Apple SDK 등 외부 구성 요소의 라이선스는 그대로 유지한다.

## Android 공통 파일 시스템 (0.5)

Android 소비 앱은 `iiSocietyContainer_configure_android_client(target)`를 호출하고 Society와 동일한 인증서로 서명한다. Society 앱의 내부 ContentProvider가 앱이 닫혀 있어도 요청에 응답한다. `fileSystem.open()`은 기존 Society UUID를 선택하며 별도 컨테이너를 만들지 않는다. `path()`와 `url()`은 Android 소비 앱에서 content URI를 반환한다. `QFile`로 읽기·쓰기를 수행하고 `ensureDirectory()`와 `entries(sectionKey, relativePath)`로 폴더를 준비·열거한다. `entries` 항목은 `name`, `path`, `isDirectory`, `size`이다. 네이티브 경로가 필요한 엔진은 URI를 자체 캐시로 복사해 사용한다. 데스크톱과 Society 소유 앱의 절대 경로 동작은 유지한다.

내부 제공자는 서명 권한과 호출 UID의 서명을 확인하고 모든 요청의 UUID·영역·상대 경로를 검증한다. 공개 Android 파일 앱에는 계속 Files 내용만 나타난다. 이 파일 시스템 IPC는 Android 앱 관측의 백그라운드 실행 제한을 없애지 않는다. `tests/android/`의 별도 Qt 앱은 실제 앱 UID에서 Helper를 통해 8개 영역의 생성·읽기·쓰기·열거·상위 경로 거부를 검사하고 logcat의 `SOCIETY_ANDROID_PEER` JSON으로 결과를 남긴다.
