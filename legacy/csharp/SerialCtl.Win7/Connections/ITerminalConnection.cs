using System;
using System.Threading.Tasks;

namespace SerialCtl.Win7.Connections
{
    internal interface ITerminalConnection : IDisposable
    {
        event EventHandler<DataReceivedEventArgs> DataReceived;
        event EventHandler<StatusEventArgs> StatusChanged;

        bool IsConnected { get; }
        Task ConnectAsync();
        Task DisconnectAsync();
        Task SendAsync(byte[] data);
    }

    internal sealed class DataReceivedEventArgs : EventArgs
    {
        public DataReceivedEventArgs(byte[] data)
        {
            Data = data;
        }

        public byte[] Data { get; private set; }
    }

    internal sealed class StatusEventArgs : EventArgs
    {
        public StatusEventArgs(string message, bool isError)
        {
            Message = message;
            IsError = isError;
        }

        public string Message { get; private set; }
        public bool IsError { get; private set; }
    }
}
