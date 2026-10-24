using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Reflection;
using System.Windows.Forms;

static class UiLayoutTest
{
    [STAThread]
    static int Main(string[] args)
    {
        Application.EnableVisualStyles();
        Assembly app = Assembly.LoadFrom(args[0]);
        using (Form form = (Form)Activator.CreateInstance(app.GetType("MMMergeAssets.BuilderForm", true)))
        {
            form.ShowInTaskbar = false;
            form.Opacity = 0;
            form.Show();
            Application.DoEvents();
            form.CreateControl();
            form.PerformLayout();
            foreach (Control control in form.Controls)
            {
                if (!form.ClientRectangle.Contains(control.Bounds))
                    throw new Exception("Control outside available client space: " + control.Text);
            }
            using (Bitmap preview = new Bitmap(form.Width, form.Height))
            {
                form.DrawToBitmap(preview, new Rectangle(Point.Empty, form.Size));
                preview.Save(args[1], ImageFormat.Png);
            }
            form.Hide();
            Console.WriteLine("PASS: GUI constructs and all controls fit; offscreen rendering only, not an interactive click test.");
        }
        return 0;
    }
}
