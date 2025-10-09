#pragma once

#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

inline bool parse_score(std::string_view text, double& value) {
    std::istringstream input{std::string(text)};
    input.imbue(std::locale::classic());
    return (input >> std::noskipws >> value) && input.eof() && std::isfinite(value);
}

inline std::string format_score(double value) {
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::setprecision(std::numeric_limits<double>::max_digits10) << value;
    return output.str();
}
