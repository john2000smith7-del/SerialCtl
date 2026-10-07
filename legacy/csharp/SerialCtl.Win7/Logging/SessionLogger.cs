using System;
using System.IO;
using System.Text;

namespace SerialCtl.Win7.Logging
{
    internal sealed class SessionLogger : IDisposable
    {
        private readonly object _sync = new object();
        private StreamWriter _writer;

        public string CurrentPath { get; private set; }

        public void Start(string mode)
        {
            Stop();

            string root = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "SerialCtl",
                "logs");
            Directory.CreateDirectory(root);
            CurrentPath = Path.Combine(root, DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".log");
            _writer = new StreamWriter(CurrentPath, false, new UTF8Encoding(false));
            _writer.AutoFlush = true;
            _writer.WriteLine("# SerialCtl session");
            _writer.WriteLine("# Started: " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
            _writer.WriteLine("# Mode: " + mode);
            _writer.WriteLine();
        }

        public void Write(string text)
        {
            lock (_sync)
            {
                _writer?.Write(text);
            }
        }

        public void WriteStatus(string text)
        {
            lock (_sync)
            {
                _writer?.WriteLine();
                _writer?.WriteLine("[" + DateTime.Now.ToString("HH:mm:ss") + "] " + text);
            }
        }

        public void Stop()
        {
            lock (_sync)
            {
                if (_writer != null)
                {
                    _writer.WriteLine();
                    _writer.WriteLine("# Ended: " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
                    _writer.Dispose();
                    _writer = null;
                }
            }
        }

        public void Dispose()
        {
            Stop();
        }
    }
}
