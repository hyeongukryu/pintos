#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init (void);

// 커널의 다른 부분에서 복구할 수 없는 유저 프로그램 오류가
// 발생했을 때 이 함수를 인자 -1로 호출하여 종료하십시오.
void exit (int);

#endif /* userprog/syscall.h */
