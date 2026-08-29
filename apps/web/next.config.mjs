/** @type {import('next').NextConfig} */
const nextConfig = {
  reactStrictMode: true,
  // The protocol package ships TypeScript source so the browser and the Node
  // broker consume one source of truth rather than a built copy that can lag.
  transpilePackages: ['@glsplay/protocol'],
};

export default nextConfig;
