import {env} from 'cloudflare:workers';
import {getChatGPTUser} from '../app/chatgpt-auth';
export async function storage(){const user=await getChatGPTUser();if(!user)throw new Error('AUTH');if(!env.DB||!env.BUCKET)throw new Error('UNAVAILABLE');return {db:env.DB,bucket:env.BUCKET,owner:user.userId}}
export function failure(e:unknown){console.error('CG storage',e instanceof Error?e.message:'unknown');return Response.json({error:e instanceof Error&&e.message==='AUTH'?'로그인이 필요합니다. 페이지를 다시 열어주세요.':'저장 서비스에 연결하지 못했습니다. 작업 내용은 유지됩니다. 다시 시도하거나 파일로 저장하세요.'},{status:e instanceof Error&&e.message==='AUTH'?401:503})}
export function sameOrigin(req:Request){const origin=req.headers.get('origin');return !origin||origin===new URL(req.url).origin||origin==='https://cg-edit-beauty.yjjang042.chatgpt.site'}
