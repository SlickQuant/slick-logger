#include <slick/logger.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

int main() {
    std::filesystem::remove("package-consumer.log");
    auto& logger = slick::logger::Logger::instance();
    logger.init("package-consumer.log", 1024);
    LOG_INFO("Package consumer value {}", 42);
    logger.shutdown();

    std::ifstream output("package-consumer.log");
    const std::string contents{std::istreambuf_iterator<char>{output},
                               std::istreambuf_iterator<char>{}};
    return contents.find("Package consumer value 42") != std::string::npos ? 0 : 1;
}
