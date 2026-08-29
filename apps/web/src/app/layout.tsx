import type { Metadata, Viewport } from 'next';
import './globals.css';

export const metadata: Metadata = {
  title: 'glsplay',
  description: 'Low-latency browser game streaming from a GCP L4 over WebRTC',
};

export const viewport: Viewport = {
  width: 'device-width',
  initialScale: 1,
  // Pinch-zooming a game viewport is never intentional.
  maximumScale: 1,
  userScalable: false,
  themeColor: '#07090b',
};

export default function RootLayout({ children }: { children: React.ReactNode }) {
  return (
    <html lang="en">
      <body className="h-full overflow-hidden bg-void text-ink antialiased">{children}</body>
    </html>
  );
}
