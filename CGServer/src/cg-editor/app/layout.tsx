import type { Metadata } from 'next';
import './globals.css';
export const metadata: Metadata = { title:'Huton CG Editor v1.0', description:'페이지, 레이어, 자막 속성을 편집하는 문자발생기 작업 화면', icons:{icon:'/favicon.svg'} };
export default function RootLayout({children}:Readonly<{children:React.ReactNode}>){return <html lang="ko"><body>{children}</body></html>}
