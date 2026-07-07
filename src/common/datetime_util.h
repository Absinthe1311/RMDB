#pragma once
#include <string>
#include <cstdint>
#include <cctype>
#include "errors.h"

// 校验并编码：将形如 'YYYY-MM-DD HH:MM:SS' 的字符串转换为内部8字节整数表示
// 编码方式：year*10^10 + month*10^8 + day*10^6 + hour*10^4 + minute*10^2 + second
// 该编码方式保证数值大小顺序与时间先后顺序完全一致，便于直接复用整数比较逻辑
inline int64_t encode_datetime(const std::string &str) {
    // 1. 长度必须严格等于19（"YYYY-MM-DD HH:MM:SS"）
    if (str.size() != 19) {
        throw DatetimeFormatError();
    }
    // 2. 分隔符位置必须严格匹配
    if (str[4] != '-' || str[7] != '-' || str[10] != ' ' || str[13] != ':' || str[16] != ':') {
        throw DatetimeFormatError();
    }
    // 3. 除分隔符外，其余位置必须全部是数字（排除负号等非法字符）
    for (int i : {0,1,2,3, 5,6, 8,9, 11,12, 14,15, 17,18}) {
        if (!isdigit(static_cast<unsigned char>(str[i]))) {
            throw DatetimeFormatError();
        }
    }

    int year   = std::stoi(str.substr(0, 4));
    int month  = std::stoi(str.substr(5, 2));
    int day    = std::stoi(str.substr(8, 2));
    int hour   = std::stoi(str.substr(11, 2));
    int minute = std::stoi(str.substr(14, 2));
    int second = std::stoi(str.substr(17, 2));

    // 4. 年份范围校验（题目要求最小值1000年，最大值9999年）
    if (year < 1000 || year > 9999) {
        throw DatetimeFormatError();
    }
    // 5. 月份范围校验
    if (month < 1 || month > 12) {
        throw DatetimeFormatError();
    }
    // 6. 日期范围校验，需要考虑每月天数与闰年2月的情形
    static const int days_in_month[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    int max_day = days_in_month[month - 1];
    bool is_leap = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
    if (month == 2 && is_leap) {
        max_day = 29;
    }
    if (day < 1 || day > max_day) {
        throw DatetimeFormatError();
    }
    // 7. 时分秒范围校验
    if (hour < 0 || hour > 23) {
        throw DatetimeFormatError();
    }
    if (minute < 0 || minute > 59) {
        throw DatetimeFormatError();
    }
    if (second < 0 || second > 59) {
        throw DatetimeFormatError();
    }

    int64_t encoded = (int64_t)year * 10000000000LL
                     + (int64_t)month * 100000000LL
                     + (int64_t)day * 1000000LL
                     + (int64_t)hour * 10000LL
                     + (int64_t)minute * 100LL
                     + (int64_t)second;
    return encoded;
}

// 解码：将内部整数表示还原为 'YYYY-MM-DD HH:MM:SS' 格式的字符串
inline std::string decode_datetime(int64_t val) {
    int second = val % 100; val /= 100;
    int minute = val % 100; val /= 100;
    int hour   = val % 100; val /= 100;
    int day    = val % 100; val /= 100;
    int month  = val % 100; val /= 100;
    int year   = (int)val;

    char buf[20];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
              year, month, day, hour, minute, second);
    return std::string(buf);
}