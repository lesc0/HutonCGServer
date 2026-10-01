import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  // 보드 LAN IP로 접속할 때(.env CG_EDITOR_HOST=0.0.0.0) HMR 리소스 차단 방지.
  // IP가 바뀌면 여기도 맞춰야 함.
  allowedDevOrigins: ["10.10.10.56"],
};

export default nextConfig;
