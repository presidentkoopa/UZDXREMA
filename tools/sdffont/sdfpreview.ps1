# [BB] Quality gate for the SDF atlas.
#
# Reconstructs glyphs from the field the way the shader will, at a
# magnification the player could actually reach by walking up to a card, and
# puts a bitmap glyph of the same nominal size beside it. If the field does
# not clearly win here it will not win in the engine either, and better to
# find that out before any C++ is written.

param(
  [string]$Dir  = 'C:\Users\Command\AppData\Local\Temp\claude\E--UZDXREMA\76030631-3473-4c10-9592-9a316966da8b\scratchpad\sdfout',
  [string]$Ttf  = 'C:\Users\Command\Desktop\freearcadefonts\PixeloidMono.ttf',
  [string]$Out  = 'C:\Users\Command\AppData\Local\Temp\claude\E--UZDXREMA\76030631-3473-4c10-9592-9a316966da8b\scratchpad\sdfpreview.png'
)

Add-Type -AssemblyName System.Drawing

$src = @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Globalization;
using System.Runtime.InteropServices;

public class SdfPrev
{
  static float[] field; static int aw, ah; static int cell, spread;
  static Dictionary<int,int[]> glyphs = new Dictionary<int,int[]>();

  public static void Load(string dir)
  {
    using (Bitmap b = new Bitmap(System.IO.Path.Combine(dir, "sdfatlas.png")))
    {
      aw = b.Width; ah = b.Height;
      BitmapData bd = b.LockBits(new Rectangle(0,0,aw,ah), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
      byte[] raw = new byte[bd.Stride*ah];
      Marshal.Copy(bd.Scan0, raw, 0, raw.Length);
      b.UnlockBits(bd);
      field = new float[aw*ah];
      for (int y = 0; y < ah; y++)
        for (int x = 0; x < aw; x++)
          field[y*aw+x] = raw[y*bd.Stride + x*4 + 2] / 255f;   // red
    }
    foreach (string ln in System.IO.File.ReadAllLines(System.IO.Path.Combine(dir,"sdfmetrics.txt")))
    {
      string[] p = ln.Split(' ');
      if (p[0] == "cell")   cell   = int.Parse(p[1]);
      if (p[0] == "spread") spread = int.Parse(p[1]);
      if (p[0] == "g")      glyphs[int.Parse(p[1])] = new int[]{ int.Parse(p[2]), int.Parse(p[3]) };
    }
  }

  // Bilinear sample of the field, in atlas pixel coords.
  static float Sample(float x, float y)
  {
    int x0 = (int)Math.Floor(x), y0 = (int)Math.Floor(y);
    float fx = x - x0, fy = y - y0;
    Func<int,int,float> at = (px,py) => {
      if (px < 0) px = 0; if (py < 0) py = 0;
      if (px >= aw) px = aw-1; if (py >= ah) py = ah-1;
      return field[py*aw+px];
    };
    float a = at(x0,y0)*(1-fx) + at(x0+1,y0)*fx;
    float b = at(x0,y0+1)*(1-fx) + at(x0+1,y0+1)*fx;
    return a*(1-fy) + b*fy;
  }

  // Draw one glyph reconstructed from the field. px = on-screen cell size.
  // glow > 0 adds the neon falloff outside the edge.
  public static void Glyph(Bitmap dst, int code, int ox, int oy, int px, Color col, float glow)
  {
    if (!glyphs.ContainsKey(code)) return;
    int[] g = glyphs[code];
    float k = (float)cell / px;              // output px -> atlas px
    float w = 0.5f * k;                      // half an output pixel, atlas units
    for (int y = 0; y < px; y++)
    for (int x = 0; x < px; x++)
    {
      int dx = ox+x, dy = oy+y;
      if (dx < 0 || dy < 0 || dx >= dst.Width || dy >= dst.Height) continue;
      float d = Sample(g[0] + x*k, g[1] + y*k);
      float sd = (d - 0.5f) * 2f * spread;   // signed distance, atlas px

      float a = Math.Max(0, Math.Min(1, (sd + w) / (2*w)));
      float gl = 0f;
      if (glow > 0f && sd < 0) gl = Math.Max(0, 1f + sd/glow);
      gl = gl*gl;

      float t = Math.Max(a, gl*0.85f);
      if (t <= 0.002f) continue;
      Color old = dst.GetPixel(dx,dy);
      int r = (int)Math.Min(255, old.R + col.R*t);
      int gg= (int)Math.Min(255, old.G + col.G*t);
      int bb= (int)Math.Min(255, old.B + col.B*t);
      dst.SetPixel(dx,dy, Color.FromArgb(255,r,gg,bb));
    }
  }

  // The comparison: a real bitmap glyph at `srcPx` then scaled up to `dstPx`,
  // which is what the engine does with a font today.
  public static void Bitmapped(Bitmap dst, string ttf, char ch, int ox, int oy, int srcPx, int dstPx)
  {
    PrivateFontCollection pfc = new PrivateFontCollection();
    pfc.AddFontFile(ttf);
    using (Bitmap small = new Bitmap(srcPx, srcPx, PixelFormat.Format32bppArgb))
    {
      using (Graphics g = Graphics.FromImage(small))
      using (GraphicsPath gp = new GraphicsPath())
      {
        g.Clear(Color.Black);
        StringFormat sf = (StringFormat)StringFormat.GenericTypographic.Clone();
        gp.AddString(ch.ToString(), pfc.Families[0], 0, srcPx*0.75f, new PointF(srcPx*0.08f, srcPx*0.08f), sf);
        g.FillPath(Brushes.White, gp);
      }
      using (Graphics g2 = Graphics.FromImage(dst))
      {
        g2.InterpolationMode = InterpolationMode.HighQualityBilinear;
        g2.PixelOffsetMode = PixelOffsetMode.Half;
        g2.DrawImage(small, new Rectangle(ox, oy, dstPx, dstPx));
      }
    }
  }

  public static void Run(string dir, string ttf, string outPath)
  {
    Load(dir);
    Bitmap dst = new Bitmap(1180, 700, PixelFormat.Format32bppArgb);
    using (Graphics g = Graphics.FromImage(dst)) g.Clear(Color.FromArgb(255, 7, 7, 10));

    Font lbl = new Font("Consolas", 11);
    Color cyan = Color.FromArgb(255, 40, 255, 255);
    Color gold = Color.FromArgb(255, 255, 225, 55);

    // Row 1 -- a string at card size, hard edged.
    string s = "CG B0001";
    for (int i = 0; i < s.Length; i++) Glyph(dst, s[i], 14 + i*62, 30, 76, cyan, 0);

    // Row 2 -- same, with glow. This is the neon.
    for (int i = 0; i < s.Length; i++) Glyph(dst, s[i], 14 + i*62, 130, 76, gold, 5.0f);

    // Row 3 -- the magnification test, side by side.
    Bitmapped(dst, ttf, 'B', 30, 260, 20, 300);          // bitmap, blown up
    Glyph(dst, 'B', 400, 260, 300, cyan, 0);             // field, same size
    Glyph(dst, 'B', 760, 260, 300, gold, 9.0f);          // field + glow

    using (Graphics g = Graphics.FromImage(dst))
    {
      g.DrawString("reconstructed from the field, card size", lbl, Brushes.Gray, 520, 55);
      g.DrawString("same, with glow -- one texture, no blur pass", lbl, Brushes.Gray, 520, 155);
      g.DrawString("BITMAP glyph upscaled  (what it does today)", lbl, Brushes.Gray, 30, 578);
      g.DrawString("FIELD, same size", lbl, Brushes.Gray, 400, 578);
      g.DrawString("FIELD + glow", lbl, Brushes.Gray, 760, 578);
      g.DrawString("300px tall -- the size a card reaches when you walk up to it", lbl, Brushes.DimGray, 30, 606);
    }
    dst.Save(outPath, ImageFormat.Png);
    dst.Dispose();
  }
}
'@

if (-not ([System.Management.Automation.PSTypeName]'SdfPrev').Type) {
  Add-Type -TypeDefinition $src -ReferencedAssemblies 'System.Drawing.dll'
}
[SdfPrev]::Run($Dir, $Ttf, $Out)
Write-Output "wrote $Out"
