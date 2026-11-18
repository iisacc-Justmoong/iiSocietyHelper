#include "../../src/Client/LocalFrame.h"
#include <QCoreApplication>

#include <array>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#define CHECK(expression) \
    do { \
        if (!(expression)) \
            throw std::runtime_error(std::string(__FILE__) + ":" + \
                                     std::to_string(__LINE__) + ": " + #expression); \
    } while (false)

using iiSocietyHelper::LocalFrame;
class LocalFrameTests final {
public:
    void fragmentedAndCoalescedFrames() {
        const QJsonObject first{{"requestId", "one"}, {"data", QString(350000, 'a')}};
        const QJsonObject second{{"requestId", "two"}};
        const auto bytes = LocalFrame::encode(first) + LocalFrame::encode(second);
        CHECK(!bytes.isEmpty());
        LocalFrame parser;
        CHECK(parser.append(bytes.first(2))); CHECK(!parser.take());
        CHECK(parser.append(bytes.mid(2, 27))); CHECK(!parser.take());
        CHECK(parser.append(bytes.mid(29)));
        CHECK(parser.take().value() == first); CHECK(parser.take().value() == second);
        CHECK(!parser.take()); CHECK(!parser.invalid());
    }
    void rejectsOversizedAndMalformedFrames() {
        CHECK(LocalFrame::encode({{"data", QString(600000, 'a')}}).isEmpty());
        LocalFrame tooLarge; CHECK(!tooLarge.append(QByteArray::fromHex("00080001"))); CHECK(tooLarge.invalid());
        LocalFrame empty; CHECK(!empty.append(QByteArray::fromHex("00000000"))); CHECK(empty.invalid());
        LocalFrame invalid; CHECK(invalid.append(QByteArray::fromHex("000000015b")));
        CHECK(!invalid.take()); CHECK(invalid.invalid());
        CHECK(!invalid.append(LocalFrame::encode({{"x", 1}})));
    }
};
int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    LocalFrameTests tests;
    const std::array cases{
        std::pair{"fragmentedAndCoalescedFrames", &LocalFrameTests::fragmentedAndCoalescedFrames},
        std::pair{"rejectsOversizedAndMalformedFrames", &LocalFrameTests::rejectsOversizedAndMalformedFrames},
    };
    int failures = 0;
    for (const auto& [name, method] : cases) {
        try {
            (tests.*method)();
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << name << ": unexpected non-standard exception\n";
        }
    }
    return failures == 0 ? 0 : 1;
}
