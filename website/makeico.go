//go:build ignore
// +build ignore

// makeico.go — 把一张大图（PNG/JPG）打包成网站 favicon.ico
//
// 用法:  go run makeico.go <源图路径> <输出 .ico 路径>
// 同时在 .ico 同目录生成 256x256 的 PNG 供网站 header/logo 复用。
//
// 依赖 golang.org/x/image/draw 做高质量双线性缩放；
// Go 1.20+ 标准库 image/jpeg / image/png 负责解码编码。

package main

import (
	"bytes"
	"fmt"
	"image"
	"image/jpeg"
	"image/png"
	"os"
	"path/filepath"

	"golang.org/x/image/draw"
)

var targetSizes = []int{16, 32, 48, 256}

type sizedPNG struct {
	size int
	data []byte
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintf(os.Stderr, "usage: go run makeico.go <input.(png|jpg)> <output.ico>\n")
		os.Exit(2)
	}
	srcPath := os.Args[1]
	icoPath := os.Args[2]
	pngPath := filepath.Join(filepath.Dir(icoPath), "favicon.png")

	src, err := openImage(srcPath)
	if err != nil {
		fatalf("open %s: %v", srcPath, err)
	}

	images := make([]sizedPNG, 0, len(targetSizes))
	for _, s := range targetSizes {
		small := scaleTo(src, s)
		buf, err := encodePNG(small)
		if err != nil {
			fatalf("encode %dx%d: %v", s, s, err)
		}
		images = append(images, sizedPNG{size: s, data: buf})
	}

	// 256x256 PNG 供网站 header/logo 复用
	if err := os.WriteFile(pngPath, images[len(images)-1].data, 0o644); err != nil {
		fatalf("write png %s: %v", pngPath, err)
	}
	fmt.Printf("PNG  written: %s (%d bytes)\n", pngPath, len(images[len(images)-1].data))

	icoData := packICO(images)
	if err := os.WriteFile(icoPath, icoData, 0o644); err != nil {
		fatalf("write ico: %v", err)
	}
	fmt.Printf("ICO  written: %s (%d bytes, entries: 16/32/48/256)\n", icoPath, len(icoData))
}

func openImage(path string) (image.Image, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()
	// 先尝试 JPEG
	img, err := jpeg.Decode(f)
	if err == nil {
		return img, nil
	}
	if _, err := f.Seek(0, 0); err != nil {
		return nil, err
	}
	return png.Decode(f)
}

func scaleTo(src image.Image, size int) *image.NRGBA {
	dst := image.NewNRGBA(image.Rect(0, 0, size, size))
	draw.BiLinear.Scale(dst, dst.Bounds(), src, src.Bounds(), draw.Over, nil)
	return dst
}

func encodePNG(img image.Image) ([]byte, error) {
	var buf bytes.Buffer
	if err := png.Encode(&buf, img); err != nil {
		return nil, err
	}
	return buf.Bytes(), nil
}

// packICO 按 MSDN ICONDIR 格式打包，图像体用 PNG（Windows Vista+ 支持）。
//
// ICONDIR       { WORD reserved=0; WORD type=2(ICO); WORD count }
// ICONDIRENTRY  { BYTE width; BYTE height; BYTE colors=0; BYTE reserved=0;
//                 WORD planes=1; WORD bpp=32; DWORD size; DWORD offset }
func packICO(images []sizedPNG) []byte {
	const headerSize = 6
	const entrySize = 16

	total := headerSize + entrySize*len(images)
	for _, im := range images {
		total += len(im.data)
	}
	out := make([]byte, 0, total)

	out = appendWord(out, 0)
	out = appendWord(out, 2) // type: ICO
	out = appendWord(out, uint16(len(images)))

	offset := uint32(headerSize + entrySize*len(images))
	for _, im := range images {
		w := byte(im.size)
		if im.size >= 256 {
			w = 0 // ICONDIRENTRY 里 0 表示 256
		}
		h := w
		out = append(out, w, h, 0, 0)
		out = appendWord(out, 1)  // planes
		out = appendWord(out, 32) // bpp
		out = appendDWord(out, uint32(len(im.data)))
		out = appendDWord(out, offset)
		offset += uint32(len(im.data))
	}

	for _, im := range images {
		out = append(out, im.data...)
	}
	return out
}

func appendWord(b []byte, v uint16) []byte {
	return append(b, byte(v), byte(v>>8))
}
func appendDWord(b []byte, v uint32) []byte {
	return append(b, byte(v), byte(v>>8), byte(v>>16), byte(v>>24))
}

func fatalf(format string, args ...any) {
	fmt.Fprintf(os.Stderr, "makeico: "+format+"\n", args...)
	os.Exit(1)
}
