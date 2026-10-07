using System;
using System.Drawing;
using System.IO;
using System.IO.Ports;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading.Tasks;
using System.Windows.Forms;
using SerialCtl.Win7.Connections;
using SerialCtl.Win7.Logging;

namespace SerialCtl.Win7
{
    internal sealed class MainForm : Form
    {
        private const int MaxTerminalCharacters = 2000000;
        private static readonly Regex AnsiRegex = new Regex(@"\x1B(?:\[[0-?]*[ -/]*[@-~]|\][^\x07]*(?:\x07|\x1B\\))", RegexOptions.Compiled);

        private readonly ComboBox _mode = NewComboBox(140);
        private readonly ComboBox _serialPort = NewComboBox(90);
        private readonly ComboBox _baudRate = NewComboBox(100);
        private readonly ComboBox _dataBits = NewComboBox(60);
        private readonly ComboBox _parity = NewComboBox(80);
        private readonly ComboBox _stopBits = NewComboBox(70);
        private readonly ComboBox _flowControl = NewComboBox(100);
        private readonly NumericUpDown _sharePort = NewPortBox(7000);
        private readonly TextBox _host = new TextBox { Width = 150, Text = "127.0.0.1" };
        private readonly NumericUpDown _networkPort = NewPortBox(7000);
        private readonly TextBox _username = new TextBox { Width = 100 };
        private readonly TextBox _password = new TextBox { Width = 120, UseSystemPasswordChar = true };
        private readonly ComboBox _encoding = NewComboBox(80);
        private readonly ComboBox _lineEnding = NewComboBox(80);
        private readonly CheckBox _stripAnsi = new CheckBox { Text = "过滤 ANSI", AutoSize = true, Checked = true };
        private readonly CheckBox _localEcho = new CheckBox { Text = "本地回显", AutoSize = true };
        private readonly Button _connectButton = new Button { Text = "连接", AutoSize = true };
        private readonly Button _disconnectButton = new Button { Text = "断开", AutoSize = true, Enabled = false };
        private readonly Button _refreshPortsButton = new Button { Text = "刷新串口", AutoSize = true };
        private readonly Button _sendButton = new Button { Text = "发送", Width = 72 };
        private readonly Button _clearButton = new Button { Text = "清屏", AutoSize = true };
        private readonly Button _saveButton = new Button { Text = "另存日志", AutoSize = true };
        private readonly RichTextBox _terminal = new RichTextBox();
        private readonly TextBox _input = new TextBox();
        private readonly ToolStripStatusLabel _statusLabel = new ToolStripStatusLabel("未连接");
        private readonly ToolStripStatusLabel _logLabel = new ToolStripStatusLabel();
        private readonly SessionLogger _logger = new SessionLogger();

        private ITerminalConnection _connection;
        private Decoder _decoder;
        private bool _closing;

        public MainForm()
        {
            Text = "SerialCtl for Windows 7 - MVP";
            StartPosition = FormStartPosition.CenterScreen;
            MinimumSize = new Size(900, 620);
            Size = new Size(1120, 760);
            Font = new Font("Microsoft YaHei UI", 9F, FontStyle.Regular, GraphicsUnit.Point, 134);

            InitializeValues();
            BuildLayout();
            WireEvents();
            RefreshSerialPorts();
            UpdateModeControls();
        }

        private void InitializeValues()
        {
            _mode.Items.AddRange(new object[] { "本地串口", "串口共享", "远程 TCP", "Telnet", "SSH" });
            _mode.SelectedIndex = 0;

            _baudRate.Items.AddRange(new object[] { "9600", "38400", "57600", "115200", "230400", "460800", "921600", "1500000" });
            _baudRate.Text = "115200";
            _dataBits.Items.AddRange(new object[] { "5", "6", "7", "8" });
            _dataBits.SelectedItem = "8";
            _parity.Items.AddRange(Enum.GetNames(typeof(Parity)));
            _parity.SelectedItem = Parity.None.ToString();
            _stopBits.Items.AddRange(new object[] { StopBits.One.ToString(), StopBits.OnePointFive.ToString(), StopBits.Two.ToString() });
            _stopBits.SelectedItem = StopBits.One.ToString();
            _flowControl.Items.AddRange(Enum.GetNames(typeof(Handshake)));
            _flowControl.SelectedItem = Handshake.None.ToString();

            _encoding.Items.AddRange(new object[] { "UTF-8", "ASCII", "GBK" });
            _encoding.SelectedIndex = 0;
            _lineEnding.Items.AddRange(new object[] { "CR", "LF", "CRLF", "无" });
            _lineEnding.SelectedItem = "CR";

            _terminal.Dock = DockStyle.Fill;
            _terminal.BackColor = Color.FromArgb(20, 20, 20);
            _terminal.ForeColor = Color.Gainsboro;
            _terminal.Font = new Font("Consolas", 10F);
            _terminal.ReadOnly = true;
            _terminal.WordWrap = false;
            _terminal.DetectUrls = false;

            _input.Dock = DockStyle.Fill;
            _sendButton.Enabled = false;
        }

        private void BuildLayout()
        {
            TableLayoutPanel root = new TableLayoutPanel
            {
                Dock = DockStyle.Fill,
                ColumnCount = 1,
                RowCount = 5,
                Padding = new Padding(6)
            };
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.Percent, 100F));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));
            root.RowStyles.Add(new RowStyle(SizeType.AutoSize));

            FlowLayoutPanel connectionRow = NewRow();
            AddLabeled(connectionRow, "模式", _mode);
            connectionRow.Controls.Add(_connectButton);
            connectionRow.Controls.Add(_disconnectButton);
            connectionRow.Controls.Add(_refreshPortsButton);
            connectionRow.Controls.Add(_clearButton);
            connectionRow.Controls.Add(_saveButton);
            connectionRow.Controls.Add(_stripAnsi);

            FlowLayoutPanel settingsRow = NewRow();
            AddLabeled(settingsRow, "串口", _serialPort);
            AddLabeled(settingsRow, "波特率", _baudRate);
            AddLabeled(settingsRow, "数据位", _dataBits);
            AddLabeled(settingsRow, "校验", _parity);
            AddLabeled(settingsRow, "停止位", _stopBits);
            AddLabeled(settingsRow, "流控", _flowControl);
            AddLabeled(settingsRow, "共享端口", _sharePort);
            AddLabeled(settingsRow, "主机", _host);
            AddLabeled(settingsRow, "端口", _networkPort);
            AddLabeled(settingsRow, "用户", _username);
            AddLabeled(settingsRow, "密码", _password);

            TableLayoutPanel sendRow = new TableLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, ColumnCount = 7 };
            sendRow.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100F));
            sendRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            sendRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            sendRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            sendRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            sendRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            sendRow.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
            sendRow.Controls.Add(_input, 0, 0);
            sendRow.Controls.Add(_sendButton, 1, 0);
            sendRow.Controls.Add(new Label { Text = "结尾", AutoSize = true, Anchor = AnchorStyles.Left }, 2, 0);
            sendRow.Controls.Add(_lineEnding, 3, 0);
            sendRow.Controls.Add(new Label { Text = "编码", AutoSize = true, Anchor = AnchorStyles.Left }, 4, 0);
            sendRow.Controls.Add(_encoding, 5, 0);
            sendRow.Controls.Add(_localEcho, 6, 0);

            StatusStrip statusStrip = new StatusStrip();
            statusStrip.Items.Add(_statusLabel);
            statusStrip.Items.Add(new ToolStripStatusLabel { Spring = true });
            statusStrip.Items.Add(_logLabel);

            root.Controls.Add(connectionRow, 0, 0);
            root.Controls.Add(settingsRow, 0, 1);
            root.Controls.Add(_terminal, 0, 2);
            root.Controls.Add(sendRow, 0, 3);
            root.Controls.Add(statusStrip, 0, 4);
            Controls.Add(root);
        }

        private void WireEvents()
        {
            _mode.SelectedIndexChanged += (sender, args) => UpdateModeControls();
            _refreshPortsButton.Click += (sender, args) => RefreshSerialPorts();
            _connectButton.Click += async (sender, args) => await ConnectAsync();
            _disconnectButton.Click += async (sender, args) => await DisconnectAsync();
            _sendButton.Click += async (sender, args) => await SendInputAsync();
            _input.KeyDown += async (sender, args) =>
            {
                if (args.KeyCode == Keys.Enter)
                {
                    args.SuppressKeyPress = true;
                    await SendInputAsync();
                }
            };
            _clearButton.Click += (sender, args) => _terminal.Clear();
            _saveButton.Click += (sender, args) => SaveTerminalText();
            FormClosing += OnFormClosing;
        }

        private async Task ConnectAsync()
        {
            SetBusy(true);
            try
            {
                _decoder = GetSelectedEncoding().GetDecoder();
                ITerminalConnection connection = CreateConnection();
                connection.DataReceived += OnConnectionDataReceived;
                connection.StatusChanged += OnConnectionStatusChanged;
                _connection = connection;

                _logger.Start(_mode.Text);
                _logLabel.Text = "日志：" + _logger.CurrentPath;
                await connection.ConnectAsync();

                _connectButton.Enabled = false;
                _disconnectButton.Enabled = true;
                _sendButton.Enabled = true;
                SetConfigurationEnabled(false);
                _input.Focus();
            }
            catch (Exception ex)
            {
                AppendStatus("连接失败：" + ex.Message, true);
                CleanupConnection();
                _logger.Stop();
            }
            finally
            {
                SetBusy(false);
            }
        }

        private async Task DisconnectAsync()
        {
            SetBusy(true);
            ITerminalConnection connection = _connection;
            try
            {
                if (connection != null)
                {
                    await connection.DisconnectAsync();
                }
            }
            catch (Exception ex)
            {
                AppendStatus("断开时发生错误：" + ex.Message, true);
            }
            finally
            {
                CleanupConnection();
                _logger.Stop();
                _connectButton.Enabled = true;
                _disconnectButton.Enabled = false;
                _sendButton.Enabled = false;
                SetConfigurationEnabled(true);
                UpdateModeControls();
                SetBusy(false);
            }
        }

        private ITerminalConnection CreateConnection()
        {
            switch (_mode.SelectedIndex)
            {
                case 0:
                    return new SerialTerminalConnection(GetSerialSettings());
                case 1:
                    return new SerialShareConnection(GetSerialSettings(), Decimal.ToInt32(_sharePort.Value));
                case 2:
                    return new TcpTerminalConnection(RequiredText(_host, "主机"), Decimal.ToInt32(_networkPort.Value), false);
                case 3:
                    return new TcpTerminalConnection(RequiredText(_host, "主机"), Decimal.ToInt32(_networkPort.Value), true);
                case 4:
                    return new SshTerminalConnection(
                        RequiredText(_host, "主机"),
                        Decimal.ToInt32(_networkPort.Value),
                        RequiredText(_username, "用户名"),
                        _password.Text);
                default:
                    throw new InvalidOperationException("请选择连接模式。");
            }
        }

        private SerialSettings GetSerialSettings()
        {
            if (string.IsNullOrWhiteSpace(_serialPort.Text))
            {
                throw new InvalidOperationException("请选择串口。");
            }

            int baudRate;
            if (!int.TryParse(_baudRate.Text, out baudRate) || baudRate <= 0)
            {
                throw new InvalidOperationException("波特率无效。");
            }

            return new SerialSettings
            {
                PortName = _serialPort.Text,
                BaudRate = baudRate,
                DataBits = int.Parse(_dataBits.Text),
                Parity = (Parity)Enum.Parse(typeof(Parity), _parity.Text),
                StopBits = (StopBits)Enum.Parse(typeof(StopBits), _stopBits.Text),
                Handshake = (Handshake)Enum.Parse(typeof(Handshake), _flowControl.Text)
            };
        }

        private async Task SendInputAsync()
        {
            ITerminalConnection connection = _connection;
            if (connection == null || !connection.IsConnected)
            {
                return;
            }

            string text = _input.Text + GetLineEnding();
            if (text.Length == 0)
            {
                return;
            }

            byte[] data = GetSelectedEncoding().GetBytes(text);
            try
            {
                await connection.SendAsync(data);
                if (_localEcho.Checked)
                {
                    AppendTerminal(text);
                }
                _input.Clear();
            }
            catch (Exception ex)
            {
                AppendStatus("发送失败：" + ex.Message, true);
            }
        }

        private void OnConnectionDataReceived(object sender, DataReceivedEventArgs e)
        {
            try
            {
                Decoder decoder = _decoder;
                if (decoder == null)
                {
                    return;
                }
                int charCount = decoder.GetCharCount(e.Data, 0, e.Data.Length, false);
                char[] chars = new char[charCount];
                decoder.GetChars(e.Data, 0, e.Data.Length, chars, 0, false);
                AppendTerminal(new string(chars));
            }
            catch (Exception ex)
            {
                AppendStatus("显示数据失败：" + ex.Message, true);
            }
        }

        private void OnConnectionStatusChanged(object sender, StatusEventArgs e)
        {
            AppendStatus(e.Message, e.IsError);
        }

        private void AppendTerminal(string text)
        {
            if (_closing)
            {
                return;
            }
            if (InvokeRequired)
            {
                BeginInvoke(new Action<string>(AppendTerminal), text);
                return;
            }

            string display = _stripAnsi.Checked ? AnsiRegex.Replace(text, string.Empty) : text;
            if (_terminal.TextLength + display.Length > MaxTerminalCharacters)
            {
                _terminal.Select(0, Math.Min(200000, _terminal.TextLength));
                _terminal.SelectedText = string.Empty;
            }
            _terminal.AppendText(display);
            _terminal.SelectionStart = _terminal.TextLength;
            _terminal.ScrollToCaret();
            _logger.Write(display);
        }

        private void AppendStatus(string message, bool isError)
        {
            if (_closing)
            {
                return;
            }
            if (InvokeRequired)
            {
                BeginInvoke(new Action<string, bool>(AppendStatus), message, isError);
                return;
            }

            _statusLabel.Text = message;
            _statusLabel.ForeColor = isError ? Color.DarkRed : SystemColors.ControlText;
            _logger.WriteStatus(message);
        }

        private void RefreshSerialPorts()
        {
            string selected = _serialPort.Text;
            string[] ports = SerialPort.GetPortNames();
            Array.Sort(ports, StringComparer.OrdinalIgnoreCase);
            _serialPort.Items.Clear();
            _serialPort.Items.AddRange(ports);
            if (!string.IsNullOrEmpty(selected) && Array.IndexOf(ports, selected) >= 0)
            {
                _serialPort.SelectedItem = selected;
            }
            else if (ports.Length > 0)
            {
                _serialPort.SelectedIndex = 0;
            }
        }

        private void UpdateModeControls()
        {
            bool serial = _mode.SelectedIndex == 0 || _mode.SelectedIndex == 1;
            bool share = _mode.SelectedIndex == 1;
            bool network = _mode.SelectedIndex >= 2;
            bool ssh = _mode.SelectedIndex == 4;

            _serialPort.Enabled = serial;
            _baudRate.Enabled = serial;
            _dataBits.Enabled = serial;
            _parity.Enabled = serial;
            _stopBits.Enabled = serial;
            _flowControl.Enabled = serial;
            _sharePort.Enabled = share;
            _host.Enabled = network;
            _networkPort.Enabled = network;
            _username.Enabled = ssh;
            _password.Enabled = ssh;

            if (_mode.SelectedIndex == 3 && _networkPort.Value == 7000)
            {
                _networkPort.Value = 23;
            }
            else if (_mode.SelectedIndex == 4 && (_networkPort.Value == 7000 || _networkPort.Value == 23))
            {
                _networkPort.Value = 22;
            }
            else if (_mode.SelectedIndex == 2 && (_networkPort.Value == 22 || _networkPort.Value == 23))
            {
                _networkPort.Value = 7000;
            }
        }

        private void SetConfigurationEnabled(bool enabled)
        {
            _mode.Enabled = enabled;
            _refreshPortsButton.Enabled = enabled;
            if (!enabled)
            {
                _serialPort.Enabled = false;
                _baudRate.Enabled = false;
                _dataBits.Enabled = false;
                _parity.Enabled = false;
                _stopBits.Enabled = false;
                _flowControl.Enabled = false;
                _sharePort.Enabled = false;
                _host.Enabled = false;
                _networkPort.Enabled = false;
                _username.Enabled = false;
                _password.Enabled = false;
            }
        }

        private void SetBusy(bool busy)
        {
            UseWaitCursor = busy;
            _connectButton.Enabled = !busy && _connection == null;
            _disconnectButton.Enabled = !busy && _connection != null;
        }

        private void CleanupConnection()
        {
            ITerminalConnection connection = _connection;
            _connection = null;
            if (connection != null)
            {
                connection.DataReceived -= OnConnectionDataReceived;
                connection.StatusChanged -= OnConnectionStatusChanged;
                connection.Dispose();
            }
        }

        private void SaveTerminalText()
        {
            using (SaveFileDialog dialog = new SaveFileDialog())
            {
                dialog.Filter = "日志文件 (*.log)|*.log|文本文件 (*.txt)|*.txt|所有文件 (*.*)|*.*";
                dialog.FileName = "serial-" + DateTime.Now.ToString("yyyyMMdd-HHmmss") + ".log";
                if (dialog.ShowDialog(this) == DialogResult.OK)
                {
                    File.WriteAllText(dialog.FileName, _terminal.Text, new UTF8Encoding(false));
                    AppendStatus("日志已保存：" + dialog.FileName, false);
                }
            }
        }

        private async void OnFormClosing(object sender, FormClosingEventArgs e)
        {
            if (_closing)
            {
                return;
            }
            _closing = true;
            e.Cancel = true;
            try
            {
                ITerminalConnection connection = _connection;
                if (connection != null)
                {
                    await connection.DisconnectAsync();
                }
            }
            catch
            {
            }
            finally
            {
                CleanupConnection();
                _logger.Dispose();
                FormClosing -= OnFormClosing;
                Close();
            }
        }

        private Encoding GetSelectedEncoding()
        {
            if (_encoding.Text == "ASCII")
            {
                return Encoding.ASCII;
            }
            if (_encoding.Text == "GBK")
            {
                return Encoding.GetEncoding(936);
            }
            return new UTF8Encoding(false, false);
        }

        private string GetLineEnding()
        {
            switch (_lineEnding.Text)
            {
                case "CR": return "\r";
                case "LF": return "\n";
                case "CRLF": return "\r\n";
                default: return string.Empty;
            }
        }

        private static string RequiredText(TextBox textBox, string name)
        {
            string value = textBox.Text.Trim();
            if (value.Length == 0)
            {
                throw new InvalidOperationException(name + "不能为空。");
            }
            return value;
        }

        private static ComboBox NewComboBox(int width)
        {
            return new ComboBox { Width = width, DropDownStyle = ComboBoxStyle.DropDownList };
        }

        private static NumericUpDown NewPortBox(int value)
        {
            return new NumericUpDown { Width = 75, Minimum = 1, Maximum = 65535, Value = value };
        }

        private static FlowLayoutPanel NewRow()
        {
            return new FlowLayoutPanel
            {
                Dock = DockStyle.Fill,
                AutoSize = true,
                WrapContents = true,
                Padding = new Padding(0, 0, 0, 4)
            };
        }

        private static void AddLabeled(FlowLayoutPanel row, string text, Control control)
        {
            row.Controls.Add(new Label { Text = text, AutoSize = true, Margin = new Padding(6, 7, 2, 0) });
            row.Controls.Add(control);
        }
    }
}
