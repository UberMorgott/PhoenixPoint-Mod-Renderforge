using System.Collections.Generic;
using System.Text;

namespace Renderforge.Localization
{
    /// <summary>RFC 4180 reader for strings.csv. No Unity: compiled into the unit tests as a linked source.</summary>
    internal static class Csv
    {
        /// <summary>A quoted field may hold commas, quotes ("" = one quote) and newlines. CR ignored, blank lines
        /// skipped, a leading BOM dropped.</summary>
        internal static List<string[]> Parse(string text)
        {
            var rows = new List<string[]>();
            var row = new List<string>();
            var cell = new StringBuilder();
            bool quoted = false;
            for (int i = text.Length > 0 && text[0] == '\uFEFF' ? 1 : 0; i < text.Length; i++)
            {
                char c = text[i];
                if (quoted)
                {
                    if (c == '"' && i + 1 < text.Length && text[i + 1] == '"') { cell.Append('"'); i++; }
                    else if (c == '"') quoted = false;
                    else cell.Append(c);
                }
                else if (c == '"') quoted = true;
                else if (c == ',') { row.Add(cell.ToString()); cell.Length = 0; }
                else if (c == '\r') { }
                else if (c == '\n')
                {
                    row.Add(cell.ToString()); cell.Length = 0;
                    if (row.Count > 1 || row[0].Length > 0) rows.Add(row.ToArray());
                    row.Clear();
                }
                else cell.Append(c);
            }
            if (cell.Length > 0 || row.Count > 0) { row.Add(cell.ToString()); rows.Add(row.ToArray()); }
            return rows;
        }
    }
}
