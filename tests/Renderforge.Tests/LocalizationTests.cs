using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using Renderforge.Localization;
using Xunit;

namespace Renderforge.Tests
{
    public class CsvTests
    {
        [Fact]
        public void QuotedCommaStaysInOneCell()
        {
            var rows = Csv.Parse("a,\"b,c\",d\n");
            Assert.Equal(new[] { "a", "b,c", "d" }, rows.Single());
        }

        [Fact]
        public void DoubledQuoteIsOneQuote()
        {
            var rows = Csv.Parse("\"say \"\"hi\"\"\",x\n");
            Assert.Equal("say \"hi\"", rows[0][0]);
        }

        [Fact]
        public void EmbeddedNewlineStaysInTheCell()
        {
            var rows = Csv.Parse("\"line1\nline2\",x\nnext,y\n");
            Assert.Equal(2, rows.Count);
            Assert.Equal("line1\nline2", rows[0][0]);
            Assert.Equal("next", rows[1][0]);
        }

        [Theory]
        [InlineData("a,b\r\nc,d\r\n")]
        [InlineData("a,b\nc,d\n")]
        [InlineData("a,b\nc,d")]
        public void CrlfAndLfParseTheSame(string text)
        {
            var rows = Csv.Parse(text);
            Assert.Equal(2, rows.Count);
            Assert.Equal(new[] { "a", "b" }, rows[0]);
            Assert.Equal(new[] { "c", "d" }, rows[1]);
        }

        [Fact]
        public void TrailingEmptyFieldIsKept()
        {
            var rows = Csv.Parse("plain,\n");
            Assert.Equal(new[] { "plain", "" }, rows.Single());
        }

        [Fact]
        public void BlankLinesAreSkipped()
        {
            var rows = Csv.Parse("a,b\n\n\nc,d\n\n");
            Assert.Equal(2, rows.Count);
        }

        [Fact]
        public void LeadingBomIsDropped()
        {
            var rows = Csv.Parse("\uFEFFKey,Russian\nx,y\n");
            Assert.Equal("Key", rows[0][0]);
        }
    }

    public class StringTableTests
    {
        private const string Header = "Key,Russian,Chinese (Simplified),French,German,Italian,Polish,Spanish\n";

        private static StringTable Table(string body)
        {
            string error;
            var table = StringTable.Parse(Header + body, out error);
            Assert.Null(error);
            return table;
        }

        [Fact]
        public void HeaderMapsToLanguageCodes()
        {
            var table = Table("");
            Assert.Equal(new[] { "ru", "zh-CN", "fr", "de", "it", "pl", "es" }, table.Languages.ToArray());
            Assert.Equal(7, table.LanguageCount);
            Assert.Equal(0, table.KeyCount);
        }

        [Fact]
        public void BadHeaderIsRejected()
        {
            string error;
            Assert.Null(StringTable.Parse("Name,Russian\nx,y\n", out error));
            Assert.Equal("header must start with Key", error);
            Assert.Null(StringTable.Parse("Key,Klingon\nx,y\n", out error));
            Assert.Equal("unknown column 'Klingon'", error);
            Assert.Null(StringTable.Parse("", out error));
        }

        [Fact]
        public void ExactCodeHits()
        {
            var table = Table("Hello,Привет,你好,Bonjour,Hallo,Ciao,Cześć,Hola\n");
            Assert.Equal("Привет", table.Lookup("Hello", "ru"));
            Assert.Equal("Hola", table.Lookup("Hello", "es"));
        }

        [Theory]
        [InlineData("zh-CN", "你好")]   // exact code
        [InlineData("zh-TW", null)]     // prefix "zh" is not a column either
        [InlineData("fr-CA", "Bonjour")] // prefix before '-'
        [InlineData("ru-RU", "Привет")]
        public void PrefixRule(string lang, string expected)
        {
            var table = Table("Hello,Привет,你好,Bonjour,Hallo,Ciao,Cześć,Hola\n");
            Assert.Equal(expected, table.Lookup("Hello", lang));
        }

        [Fact]
        public void EmptyCellFallsBack()
        {
            var table = Table("Hello,,你好,,Hallo,Ciao,Cześć,Hola\n");
            Assert.Null(table.Lookup("Hello", "ru"));
            Assert.Null(table.Lookup("Hello", "fr"));
            Assert.Equal("Hallo", table.Lookup("Hello", "de"));
        }

        [Fact]
        public void UnknownLanguageOrKeyFallsBack()
        {
            var table = Table("Hello,Привет,你好,Bonjour,Hallo,Ciao,Cześć,Hola\n");
            Assert.Null(table.Lookup("Hello", "en"));
            Assert.Null(table.Lookup("Hello", "ja"));
            Assert.Null(table.Lookup("Missing", "ru"));
            Assert.Null(table.Lookup(null, "ru"));
            Assert.Null(table.Lookup("Hello", null));
        }

        [Fact]
        public void ShortRowBeyondItsCellsFallsBack()
        {
            var table = Table("Hello,Привет\n");
            Assert.Equal("Привет", table.Lookup("Hello", "ru"));
            Assert.Null(table.Lookup("Hello", "es"));
        }
    }

    /// <summary>The shipped strings.csv, read exactly the way the mod reads it (embedded, UTF-8, BOM-detecting).</summary>
    public class StringsCsvContractTests
    {
        private static readonly string[] ExpectedHeader =
            { "Key", "Russian", "Chinese (Simplified)", "French", "German", "Italian", "Polish", "Spanish" };

        /// <summary>DLSS preset names that stay English in the Russian UI on purpose.</summary>
        private static readonly HashSet<string> EnOnlyKeys = new HashSet<string>
            { "Native AA", "Balanced", "Performance", "Ultra Performance", "Ultra Quality", "Ultra Quality Plus" };

        private static string Text()
        {
            using (var stream = typeof(StringsCsvContractTests).Assembly.GetManifestResourceStream("Renderforge.strings.csv"))
            using (var reader = new StreamReader(stream, Encoding.UTF8, true))
                return reader.ReadToEnd();
        }

        private static List<string[]> Rows()
        {
            return Csv.Parse(Text());
        }

        [Fact]
        public void HeaderIsExact()
        {
            Assert.Equal(ExpectedHeader, Rows()[0]);
        }

        [Fact]
        public void TableBuildsWithAllLanguages()
        {
            string error;
            var table = StringTable.Parse(Text(), out error);
            Assert.Null(error);
            Assert.Equal(7, table.LanguageCount);
            Assert.True(table.KeyCount >= 100, "keys: " + table.KeyCount);
        }

        [Fact]
        public void EveryRowHasEveryColumn()
        {
            var rows = Rows().Skip(1).ToList();
            Assert.True(rows.Count >= 100, "rows: " + rows.Count);
            Assert.All(rows, row => Assert.Equal(ExpectedHeader.Length, row.Length));
        }

        [Fact]
        public void KeysAreNonEmptyAndUnique()
        {
            var keys = Rows().Skip(1).Select(r => r[0]).ToList();
            Assert.All(keys, k => Assert.False(string.IsNullOrWhiteSpace(k)));
            Assert.Empty(keys.GroupBy(k => k).Where(g => g.Count() > 1).Select(g => g.Key));
        }

        [Fact]
        public void MachineTranslatedColumnsHaveNoEmptyCell()
        {
            var rows = Rows().Skip(1).ToList();
            for (int col = 2; col < ExpectedHeader.Length; col++)
            {
                var empty = rows.Where(r => r[col].Length == 0).Select(r => r[0]).ToList();
                Assert.True(empty.Count == 0, ExpectedHeader[col] + " empty for: " + string.Join(", ", empty));
            }
        }

        [Fact]
        public void RussianIsEmptyOnlyForTheEnOnlyKeys()
        {
            var empty = Rows().Skip(1).Where(r => r[1].Length == 0).Select(r => r[0]).ToHashSet();
            Assert.Equal(EnOnlyKeys.OrderBy(k => k), empty.OrderBy(k => k));
        }

        [Fact]
        public void NoCellContainsCarriageReturn()
        {
            Assert.All(Rows().SelectMany(r => r), cell => Assert.DoesNotContain('\r', cell));
        }

        /// <summary>Every Graphics-panel image row (GradePanel.Knobs + Sharpness, LUT strength, the reset row) has its
        /// label AND its exact English tooltip (the literal from GradePanel.cs / GraphicsPanel.cs / LutPanel.cs) as a key,
        /// so no language falls back to the inline EN/RU pair.</summary>
        [Theory]
        [InlineData("Exposure", "Exposure in tenths of a stop: 10 = +1 EV (twice the light), -10 = half. Applied first in the chain. Scene only; the HUD stays unchanged. Default: 0.")]
        [InlineData("Black point", "Pixels darker than this become black and the rest stretch — deeper shadows. 0 = off. Default: 0.")]
        [InlineData("White point", "Pixels brighter than this become white — brighter highlights. 255 = off. Default: 255.")]
        [InlineData("Brightness", "Midtone brightness (gamma): black and white stay put, above 0 lifts the mids, below 0 sinks them. Scene only; the HUD stays unchanged. Default: 0.")]
        [InlineData("Contrast", "Pivots at mid-grey: 100 = off, below flattens, above deepens (dark scenes get darker). Default: 100.")]
        [InlineData("Clarity", "Local contrast on fine detail; 0 = off. Scene only; the HUD stays unchanged. Default: 0.")]
        [InlineData("Vibrance", "Saturation boost for muted colours only; vivid ones barely change. Below 0 mutes them. Scene only; the HUD stays unchanged. Default: 0.")]
        [InlineData("Saturation", "Colour intensity of everything: 0 = greyscale, 100 = as rendered, 200 = double. Scene only; the HUD stays unchanged. Default: 100.")]
        [InlineData("Sharpness", "RCAS sharpening after reconstruction; 0 = off. Default: 40.")]
        [InlineData("LUT strength", "0 = original image … 100 = full grade. Applied live. Default: 100.")]
        [InlineData("Reset image settings", "Resets Sharpness, LUT strength, Exposure, Black point, White point, Brightness, Contrast, Clarity, Vibrance and Saturation to their defaults. The LUT filter, colour vision and scene style stay as chosen.")]
        public void ImageRowsHaveLabelAndExactTooltipKeys(string label, string tooltip)
        {
            var keys = Rows().Skip(1).Select(r => r[0]).ToList();
            Assert.Contains(label, keys);
            Assert.Contains(tooltip, keys);
        }

        /// <summary>The reset row's button text is the short verb (GradePanel.BuildReset), not the row label.</summary>
        [Fact]
        public void ResetButtonVerbHasExactKey()
        {
            Assert.Contains("Reset", Rows().Skip(1).Select(r => r[0]));
        }
    }
}
