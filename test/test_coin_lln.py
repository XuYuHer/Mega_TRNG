import io
import random
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import coin_lln as coin


def sample_frame(sequence=0, value=0x55, mode=1):
    payload = coin.METADATA.pack(12345, 7680, 1920, mode, 0) + bytes([value]) * 240
    return coin.frame_bytes(payload, sequence)


class ShortReader(io.BytesIO):
    def read(self, size=-1):
        return super().read(min(size, 3))


class CoinTests(unittest.TestCase):
    def test_popcount_all_offsets_and_lengths(self):
        data = bytes(range(256))
        bits = [(b >> i) & 1 for b in data for i in range(8)]
        for offset in range(20):
            for length in range(80):
                self.assertEqual(coin.count_bits(memoryview(data), offset, length),
                                 sum(bits[offset:offset + length]))

    def test_exact_non_byte_counts_and_checkpoints(self):
        data = random.Random(9).randbytes(200)
        for count in (1, 7, 8, 9, 10, 100, 997, 1000):
            rows, summary = coin.analyze_chunks([data], count, 3)
            for row in rows:
                expected = sum((data[i // 8] >> (i % 8)) & 1 for i in range(row["flips"]))
                self.assertEqual(row["heads"], expected)
            self.assertTrue(set(coin.checkpoint_sizes(count)).issubset(r["flips"] for r in rows))
            self.assertEqual(rows[-1]["flips"], count)
            self.assertEqual(summary["heads"] + summary["tails"], count)

    def test_chunk_splitting_does_not_change_statistics(self):
        data = random.Random(10).randbytes(2000)
        expected = coin.analyze_chunks([data], 15003)
        actual = coin.analyze_chunks((data[i:i + 3] for i in range(0, len(data), 3)), 15003)
        self.assertEqual(actual, expected)

    def test_extreme_inputs_and_short_input(self):
        for byte, proportion in ((0, 0), (255, 1)):
            _, summary = coin.analyze_chunks([bytes([byte]) * 10], 79)
            self.assertEqual(summary["proportion"], proportion)
        with self.assertRaisesRegex(ValueError, "ended after"):
            coin.analyze_chunks([b"\xff"], 9)
        for count, stride in ((0, None), (10, 0), (10, -1)):
            with self.assertRaises(ValueError):
                coin.analyze_chunks([b"\xff"], count, stride)

    def test_crc_known_vector(self):
        self.assertEqual(coin.crc16_ccitt(b"123456789"), 0x29B1)

    def test_frame_split_startup_and_sequence_wrap(self):
        reader = coin.FrameReader(ShortReader(b"startup\r\n" + sample_frame(0xFFFFFFFF) + sample_frame(0)))
        frames = list(reader.frames())
        self.assertEqual([f.sequence for f in frames], [0xFFFFFFFF, 0])
        self.assertEqual(reader.resync_bytes, 9)

    def test_bad_crc_is_fatal(self):
        damaged = bytearray(sample_frame())
        damaged[40] ^= 1
        with self.assertRaisesRegex(coin.ProtocolError, "CRC"):
            list(coin.FrameReader(io.BytesIO(damaged + sample_frame(1))).frames())

    def test_lost_duplicate_reset_or_boundary_is_fatal(self):
        for next_sequence in (0, 2, 5):
            with self.assertRaisesRegex(coin.ProtocolError, "sequence"):
                list(coin.FrameReader(io.BytesIO(sample_frame() + sample_frame(next_sequence))).frames())
        with self.assertRaisesRegex(coin.ProtocolError, "boundary"):
            list(coin.FrameReader(io.BytesIO(sample_frame() + b"x" + sample_frame(1))).frames())

    def test_truncated_frame_and_invalid_header(self):
        for end in (1, 7, 12, 100, len(sample_frame()) - 1):
            with self.assertRaisesRegex(coin.ProtocolError, "truncated"):
                list(coin.FrameReader(ShortReader(sample_frame()[:end])).frames())
        for position, value in ((4, 2), (5, 7), (6, 255)):
            data = bytearray(sample_frame())
            data[position] = value
            with self.assertRaises(coin.ProtocolError):
                list(coin.FrameReader(io.BytesIO(data)).frames())

    def test_serial_timeout_and_empty_reads(self):
        with patch.object(coin.time, "monotonic", side_effect=[0, 0, 1, 11]):
            with self.assertRaisesRegex(coin.ProtocolError, "timeout"):
                list(coin.FrameReader(io.BytesIO(), live=True).frames())

    def test_collector_metadata_excluded_and_replay_capture(self):
        capture = io.BytesIO()
        wire = sample_frame() + sample_frame(1, 0xFF)
        source = coin.CollectorSource(coin.FrameReader(ShortReader(wire)), capture, "fast")
        _, summary = coin.analyze_chunks(source.chunks(), 240 * 8 + 3)
        self.assertEqual(summary["heads"], 960 + 3)
        self.assertEqual(capture.getvalue(), wire)
        metrics = source.statistics(2, replay=True)
        self.assertEqual(metrics["board_generation_us"], 2 * 12345)
        self.assertEqual(metrics["received_random_bytes"], 480)
        self.assertIsNone(metrics["host_bytes_per_second"])

    def test_board_health_and_mode_errors(self):
        for code in (1, 2, 3):
            source = coin.CollectorSource(coin.FrameReader(io.BytesIO(coin.frame_bytes(bytes([code]), flags=1))))
            with self.assertRaisesRegex(coin.ProtocolError, "Mega:"):
                list(source.chunks())
        with self.assertRaisesRegex(coin.ProtocolError, "requested"):
            list(coin.CollectorSource(coin.FrameReader(io.BytesIO(sample_frame(mode=2))),
                                      expected_mode="fast").chunks())
        with self.assertRaisesRegex(coin.ProtocolError, "changed"):
            list(coin.CollectorSource(coin.FrameReader(io.BytesIO(sample_frame() + sample_frame(1, mode=2)))).chunks())

    def test_seed_reproducible_and_cli_replay(self):
        a = b"".join(coin.iter_simulated_bytes(1000, 7))
        self.assertEqual(a, b"".join(coin.iter_simulated_bytes(1000, 7)))
        for length in range(1, 32):
            self.assertEqual(a[:length], b"".join(coin.iter_simulated_bytes(length, 7)))
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "data.mlln").write_bytes(sample_frame())
            with patch("sys.stdout", new_callable=io.StringIO):
                result = coin.main(["--input", str(root / "data.mlln"), "--flips", "1001",
                                    "--csv", str(root / "nested/result.csv"),
                                    "--json", str(root / "nested/result.json")])
            self.assertEqual(result, 0)
            self.assertIn("1001,", (root / "nested/result.csv").read_text())
            self.assertIn('"flips": 1001', (root / "nested/result.json").read_text())

    def test_serial_cli_starts_stops_and_saves_only_valid_frames(self):
        class SerialStub(ShortReader):
            def __init__(self, data):
                super().__init__(data)
                self.commands = []

            @property
            def in_waiting(self):
                return len(self.getvalue()) - self.tell()

            def write(self, data):
                self.commands.append(data)
                return len(data)

            def flush(self):
                pass

            def reset_input_buffer(self):
                pass

        for corrupt in (False, True):
            wire = bytearray(sample_frame())
            if corrupt:
                wire[-1] ^= 1
            device = SerialStub(bytes(wire))
            serial = types.ModuleType("serial")
            serial.Serial = lambda *a, **kw: device
            serial.tools = types.ModuleType("serial.tools")
            serial.tools.list_ports = types.ModuleType("serial.tools.list_ports")
            modules = {"serial": serial, "serial.tools": serial.tools,
                       "serial.tools.list_ports": serial.tools.list_ports}
            with tempfile.TemporaryDirectory() as folder:
                root = Path(folder)
                with patch.dict(sys.modules, modules), patch("sys.stdout", new_callable=io.StringIO), \
                        patch("sys.stderr", new_callable=io.StringIO):
                    code = coin.main(["--serial", "FAKE", "--boot-delay", "0", "--flips", "1001",
                                      "--capture", str(root / "data.mlln"), "--json", str(root / "out.json")])
                self.assertEqual(code, 1 if corrupt else 0)
                self.assertEqual(device.commands[0], b"F")
                self.assertEqual(device.commands[-1], b"S")
                self.assertTrue(device.closed)
                self.assertEqual((root / "out.json").exists(), not corrupt)
                self.assertEqual((root / "data.mlln").read_bytes(), b"" if corrupt else wire)


if __name__ == "__main__":
    unittest.main()
