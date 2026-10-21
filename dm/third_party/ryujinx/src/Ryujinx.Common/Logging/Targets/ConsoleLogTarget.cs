using Ryujinx.Common.Logging.Formatters;
using System;

namespace Ryujinx.Common.Logging.Targets
{
    public class ConsoleLogTarget : ILogTarget
    {
        private readonly DefaultLogFormatter _formatter;

        private readonly string _name;

        string ILogTarget.Name { get => _name; }

        private static ConsoleColor GetLogColor(LogLevel level) => level switch
        {
            LogLevel.Info => ConsoleColor.White,
            LogLevel.Warning => ConsoleColor.Yellow,
            LogLevel.Error => ConsoleColor.Red,
            LogLevel.Stub => ConsoleColor.DarkGray,
            LogLevel.Notice => ConsoleColor.Cyan,
            LogLevel.Trace => ConsoleColor.DarkCyan,
            _ => ConsoleColor.Gray,
        };

        public ConsoleLogTarget(string name)
        {
            _formatter = new DefaultLogFormatter();
            _name = name;
        }

        public void Log(object sender, LogEventArgs args)
        {
            // iOS 无终端颜色接口；日志线程的未捕获异常会终止整个游戏，仍保留文本输出。
            if (!OperatingSystem.IsIOS()) Console.ForegroundColor = GetLogColor(args.Level);
            Console.WriteLine(_formatter.Format(args));
            if (!OperatingSystem.IsIOS()) Console.ResetColor();
        }

        public void Dispose()
        {
            GC.SuppressFinalize(this);
            if (!OperatingSystem.IsIOS()) Console.ResetColor();
        }
    }
}
