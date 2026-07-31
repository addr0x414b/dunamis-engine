#ifndef RESULT_H
#define RESULT_H

#include <string>
#include <utility>

class Result final {
public:
    [[nodiscard]] static Result success() {
        return Result(true, {});
    }

    [[nodiscard]] static Result failure(std::string error) {
        return Result(false, std::move(error));
    }

    explicit operator bool() const noexcept {
        return succeeded_;
    }

    [[nodiscard]] const std::string& error() const noexcept {
        return error_;
    }

private:
    Result(bool succeeded, std::string error)
        : succeeded_(succeeded), error_(std::move(error)) {}

    bool succeeded_;
    std::string error_;
};

#endif
