import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  // 보드 LAN IP로 접속할 때(.env CG_EDITOR_HOST=0.0.0.0) HMR 리소스 차단 방지.
  // 접속 IP(공유기/DHCP)가 바뀌어도 되도록 IPv4 주소 전체를 허용 (Next 에는 "전체 허용" 옵션이 없음).
  allowedDevOrigins: ["*.*.*.*"],
};

export default nextConfig;
