using System.Collections.Generic;

namespace Renderforge.Localization
{
    /// <summary>EN key -> per-language cells, built from strings.csv. Header = Key + TFTV's language names; a language
    /// is looked up by its I2 code exactly, then by the part before '-'. No Unity: shared with the unit tests.</summary>
    internal sealed class StringTable
    {
        internal static readonly Dictionary<string, string> ColumnCodes = new Dictionary<string, string>
        {
            { "Russian", "ru" }, { "Chinese (Simplified)", "zh-CN" }, { "French", "fr" }, { "German", "de" },
            { "Italian", "it" }, { "Polish", "pl" }, { "Spanish", "es" }
        };

        private readonly Dictionary<string, int> columnByCode;      // I2 language code -> cell index
        private readonly Dictionary<string, string[]> rows;         // EN key -> row cells

        private StringTable(Dictionary<string, int> columnByCode, Dictionary<string, string[]> rows)
        {
            this.columnByCode = columnByCode; this.rows = rows;
        }

        internal int KeyCount { get { return rows.Count; } }
        internal int LanguageCount { get { return columnByCode.Count; } }
        internal IEnumerable<string> Languages { get { return columnByCode.Keys; } }

        /// <summary>Null and a reason when the header is not "Key" + known language names.</summary>
        internal static StringTable Parse(string csv, out string error)
        {
            error = null;
            var parsed = Csv.Parse(csv);
            if (parsed.Count == 0 || parsed[0][0] != "Key") { error = "header must start with Key"; return null; }
            var cols = new Dictionary<string, int>();
            for (int i = 1; i < parsed[0].Length; i++)
            {
                string code;
                if (!ColumnCodes.TryGetValue(parsed[0][i], out code)) { error = "unknown column '" + parsed[0][i] + "'"; return null; }
                cols[code] = i;
            }
            var table = new Dictionary<string, string[]>();
            for (int r = 1; r < parsed.Count; r++)
                if (parsed[r].Length > 1 && parsed[r][0].Length > 0) table[parsed[r][0]] = parsed[r];
            return new StringTable(cols, table);
        }

        /// <summary>The non-empty cell for (key, language code); null when the key, the language or the cell is missing.</summary>
        internal string Lookup(string en, string lang)
        {
            string[] row; int col;
            if (en != null && lang != null && rows.TryGetValue(en, out row) && ColumnOf(lang, out col)
                && col < row.Length && row[col].Length > 0)
                return row[col];
            return null;
        }

        private bool ColumnOf(string lang, out int col)
        {
            if (columnByCode.TryGetValue(lang, out col)) return true;
            int dash = lang.IndexOf('-');
            return dash > 0 && columnByCode.TryGetValue(lang.Substring(0, dash), out col);
        }
    }
}
