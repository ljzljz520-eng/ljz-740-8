// 示例命令：把 OBJ 模型按指定比例简化后保存为新的 OBJ。
//
// 用法:
//
//	simplify -in input.obj -out output.obj -ratio 0.25 [-lib path/to/libmeshlib.so]
//
// 简化过程中每收到进度更新会打印一行；按下 Ctrl+C（SIGINT）可
// 取消任务，此时底层 C 工作区资源立即释放，程序以非零状态退出。
package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"os"
	"os/signal"
	"syscall"

	"meshbind/mesh"
)

func main() {
	if err := run(os.Args[1:]); err != nil {
		fmt.Fprintln(os.Stderr, "error:", err)
		os.Exit(1)
	}
}

func run(args []string) error {
	fs := flag.NewFlagSet("simplify", flag.ContinueOnError)
	in := fs.String("in", "", "输入 OBJ 文件路径")
	out := fs.String("out", "", "输出 OBJ 文件路径")
	ratio := fs.Float64("ratio", 0.5, "简化后面片数占原面片数的比例，范围 (0,1]")
	libPath := fs.String("lib", "", "meshlib 共享库路径（默认自动搜索）")
	if err := fs.Parse(args); err != nil {
		return err
	}
	if *in == "" || *out == "" {
		fs.Usage()
		return errors.New("必须同时指定 -in 和 -out")
	}

	// 1. 加载本地 mesh 库
	lib, err := mesh.LoadLibrary(*libPath)
	if err != nil {
		return err
	}
	defer lib.Close()
	fmt.Println("meshlib version:", lib.Version())

	// 2. 读取模型
	s, err := lib.OpenModel(*in)
	if err != nil {
		return err
	}
	defer s.Close() // 无论成功、失败还是取消，底层网格资源都被回收
	fmt.Printf("loaded: %d vertices, %d faces\n", s.Vertices(), s.Faces())

	// 3. 设置简化比例与进度回调
	if err := s.SetRatio(*ratio); err != nil {
		return err
	}
	s.SetProgress(func(p int) (cancel bool) {
		fmt.Printf("\rprogress: %3d%%", p)
		if p >= 100 {
			fmt.Println()
		}
		return false
	})

	// 4. Ctrl+C 取消
	ctx, stop := signal.NotifyContext(context.Background(),
		syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	// 5. 执行简化
	err = s.SimplifyContext(ctx)
	if errors.Is(err, mesh.ErrCanceled) {
		fmt.Fprintln(os.Stderr, "\n任务已取消，底层资源已释放")
		return err
	}
	if err != nil {
		return err
	}
	fmt.Printf("simplified: %d vertices, %d faces\n", s.Vertices(), s.Faces())

	// 6. 导出结果
	if err := s.Export(*out); err != nil {
		return err
	}
	fmt.Println("saved to:", *out)
	return nil
}
