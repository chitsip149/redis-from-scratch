package core

import (
	"bytes"
	"errors"
	"fmt"
)

const CRLF string = "\r\n"

var RespNil = []byte("$-1\r\n")

// +OK\r\n => OK, 5
func readSimpleString(data []byte) (string, int, error) {
	idx := 1
	for data[idx] != '\r' {
		idx++
	}
	return string(data[1:idx]), idx + 2, nil
}

// :123\r\n => 123
func readInt64(data []byte) (int64, int, error) {
	idx := 1
	var res int64 = 0
	var sign int64 = 1
	if data[idx] == '-' {
		sign = -1
		idx++
	} else if data[idx] == '+' {
		idx++
	}
	for data[idx] != '\r' {
		res = res*10 + int64(data[idx]-'0')
		idx++
	}
	return res * int64(sign), idx + 2, nil
}

func readError(data []byte) (string, int, error) {
	return readSimpleString(data)
}

// $5\r\nhello\r\n => 5, 4
func readLen(data []byte) (int, int) {
	res, pos, _ := readInt64(data)
	return int(res), pos
}

func readBulkString(data []byte) (string, int, error) {
	length, idx := readLen(data)
	return string(data[idx : idx+length]), idx + length + 2, nil
}

// *2\r\n$5\r\rhello\r\n$5\r\nworld\r\n => {"hello", "world"}
func readArray(data []byte) (interface{}, int, error) {
	length, idx := readLen(data)
	var res []interface{} = make([]interface{}, length)

	for i := range length {
		val, pos, err := DecodeOne(data[idx:])
		if err != nil {
			return nil, 0, err
		}
		fmt.Println(val, pos)
		fmt.Println(string(data))
		res[i] = val
		idx += pos
	}

	return res, idx, nil
}

func DecodeOne(data []byte) (interface{}, int, error) {
	if len(data) == 0 {
		return nil, 0, errors.New("no data")
	}

	switch data[0] {
	case '+':
		return readSimpleString(data)
	case ':':
		return readInt64(data)
	case '-':
		return readError(data)
	case '$':
		return readBulkString(data)
	case '*':
		return readArray(data)
	}
	return nil, 0, nil
}

func Decode(data []byte) (interface{}, error) {
	res, _, err := DecodeOne(data)
	return res, err
}

func encodeString(s string) []byte {
	return []byte(fmt.Sprintf("$%d\r\n%s\r\n", len(s), s))
}

func encodeStringArray(sa []string) []byte {
	var b []byte
	buf := bytes.NewBuffer(b)
	for _, s := range sa {
		buf.Write(encodeString(s))
	}
	return []byte(fmt.Sprintf("*%d\r\n%s", len(sa), buf.Bytes()))
}

func Encode(value interface{}, isSimpleString bool) []byte {
	switch v := value.(type) {
	case string:
		if isSimpleString {
			return []byte(fmt.Sprintf("+%s%s", v, CRLF))
		}
		return []byte(fmt.Sprintf("$%d%s%s%s", len(v), CRLF, v, CRLF))
	case int64, int32, int16, int8, int:
		return []byte(fmt.Sprintf(":%d%s", v, CRLF))
	case error:
		return []byte(fmt.Sprintf("-%s%s", v, CRLF))
	case []string:
		return encodeStringArray(value.([]string))
	case [][]string:
		var b []byte
		buf := bytes.NewBuffer(b)
		for _, sa := range value.([][]string) {
			buf.Write(encodeStringArray(sa))
		}
		return []byte(fmt.Sprintf("*%d%s%s", len(value.([][]string)), CRLF, buf.Bytes()))
	case []interface{}:
		var b []byte
		buf := bytes.NewBuffer(b)
		for _, x := range value.([]interface{}) {
			buf.Write(Encode(x, false))
		}
		return []byte(fmt.Sprintf("*%d%s%s", len(value.([]interface{})), CRLF, buf.Bytes()))
	default:
		return RespNil
	}
}
