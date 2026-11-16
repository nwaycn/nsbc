package main

import (
	"bufio"
	"os"
	"strconv"
	"strings"
)

// Config 是 website 的运行时配置，支持 INI 文件 + flag 覆盖。
type Config struct {
	Addr string `ini:"general.addr"` // HTTP 监听地址，默认 127.0.0.1:8080

	ContentDir string `ini:"paths.content"`
	DBDir      string `ini:"paths.db"`
	TplDir     string `ini:"paths.templates"`
	AssetsDir  string `ini:"paths.assets"`

	// ACME (Let's Encrypt / ZeroSSL) 自动证书签发与后台续签。
	// enable=true 时：certmagic 自动监听 ":443" (HTTPS) + ":80" (challenge + 301 跳转)。
	ACMEEnable  bool   `ini:"acme.enable"`
	ACMEDomain  string `ini:"acme.domain"`
	ACMEEmail   string `ini:"acme.email"`
	ACMECache   string `ini:"acme.cache_dir"` // 留空则用 ~/.local/share/certmagic
	ACMEStaging bool   `ini:"acme.staging"`   // true 用 Let's Encrypt staging 环境测试
}

// DefaultConfig 返回内置默认值，当 ini 文件缺失或未提供某项时使用。
func DefaultConfig() Config {
	return Config{
		Addr:       "127.0.0.1:8080",
		ContentDir: "content",
		DBDir:      "data/site.db",
		TplDir:     "templates",
		AssetsDir:  "assets",
		ACMEEnable: false,
		ACMEDomain: "nsbc.nway.com.cn",
		ACMEEmail:  "1354608370@qq.com",
		ACMECache:  "",
	}
}

// LoadINI 从 INI 文件中读取配置，返回覆盖后的 Config。
// 文件不存在时返回默认值（不报错），方便部署场景缺省 ini 也能跑。
func LoadINI(path string) (Config, error) {
	cfg := DefaultConfig()

	f, err := os.Open(path)
	if err != nil {
		if os.IsNotExist(err) {
			return cfg, nil
		}
		return cfg, err
	}
	defer f.Close()

	section := ""
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, ";") || strings.HasPrefix(line, "#") {
			continue
		}
		if strings.HasPrefix(line, "[") && strings.HasSuffix(line, "]") {
			section = strings.TrimSpace(line[1 : len(line)-1])
			continue
		}
		eq := strings.Index(line, "=")
		if eq < 0 {
			continue
		}
		key := strings.TrimSpace(line[:eq])
		val := strings.TrimSpace(line[eq+1:])
		// 剥离行尾注释：如果首字符不是引号，就找第一个 ; 或 # 截断。
		// 引号内的值允许含分号（极少见场景）。
		if len(val) > 0 && val[0] != '"' && val[0] != '\'' {
			for _, c := range []byte{';', '#'} {
				if idx := strings.IndexByte(val, c); idx >= 0 {
					val = val[:idx]
				}
			}
			val = strings.TrimSpace(val)
		}
		val = strings.Trim(val, "\"'")
		fullKey := section + "." + key

		switch fullKey {
		case "general.addr":
			cfg.Addr = val
		case "paths.content":
			cfg.ContentDir = val
		case "paths.db":
			cfg.DBDir = val
		case "paths.templates":
			cfg.TplDir = val
		case "paths.assets":
			cfg.AssetsDir = val
		case "acme.enable":
			cfg.ACMEEnable = parseBool(val)
		case "acme.domain":
			cfg.ACMEDomain = val
		case "acme.email":
			cfg.ACMEEmail = val
		case "acme.cache_dir":
			cfg.ACMECache = val
		case "acme.staging":
			cfg.ACMEStaging = parseBool(val)
		}
	}
	return cfg, scanner.Err()
}

func parseBool(s string) bool {
	b, _ := strconv.ParseBool(strings.ToLower(s))
	return b
}
