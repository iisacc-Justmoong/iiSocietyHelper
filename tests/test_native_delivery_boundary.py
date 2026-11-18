from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]

class NativeDeliveryBoundary(unittest.TestCase):
    def test_delivery_uses_the_native_provider_database(self):
        source = (ROOT / 'src/Delivery/DeliveryStore.cpp').read_text()
        self.assertIn('iiFileProviderDatabase.h', source)
        self.assertIn('iiFileProvider::native::sql', source)
        self.assertNotRegex(source, r'\bQSql(?:Database|Query|Error)\b|<QSql')

if __name__ == '__main__':
    unittest.main()
