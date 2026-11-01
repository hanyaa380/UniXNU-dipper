"""Execute the real iOS16 local-store adapter with native Objective-C objects."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Ios16LogStore(unittest.TestCase):
    def test_abi_ownership_native_denial_and_late_completion(self):
        source = r'''
#import <Foundation/Foundation.h>
#include <objc/runtime.h>
#include <assert.h>
static unsigned mode,initialState,prepares;
static bool factoryAvailable=true;
static __weak id lastSource;
static void (^pending)(id,NSError *);
@interface FixtureSource : NSObject @end
@implementation FixtureSource @end
@interface FixtureFactory : NSObject
+(id)localStore;
-(id)prepareWithCompletionHandler:(void (^)(id,NSError *))completion;
@end
@implementation FixtureFactory
+(id)localStore {return [self new];}
-(id)prepareWithCompletionHandler:(void (^)(id,NSError *))completion {
 ++prepares;
 if(mode==1)completion(nil,[NSError errorWithDomain:@"native.logging.denial" code:13 userInfo:nil]);
 else if(mode==2)completion([NSObject new],nil);
 else if(mode==3)pending=[completion copy];
 else if(mode==4)completion(nil,nil);
 else {id s=[FixtureSource new];lastSource=s;completion(s,nil);}
 return nil;
}
@end
@interface FixtureStore : NSObject {id _source;int _constraint;}
-(instancetype)initForFactory;
@end
@implementation FixtureStore
-(instancetype)initForFactory {
 if((self=[super init])){_constraint=initialState==1?1:0;if(initialState==2)_source=[NSObject new];}
 return self;
}
@end
@interface WrongTypeStore : NSObject {id _source;long _constraint;} @end
@implementation WrongTypeStore @end
@interface MissingInitStore : NSObject {id _source;int _constraint;} @end
@implementation MissingInitStore @end
static Class lookup(NSString *name){
 if([name isEqualToString:@"OSLogEventStore"])return factoryAvailable?[FixtureFactory class]:Nil;
 if([name isEqualToString:@"OSLogEventSource"])return [FixtureSource class];
 return Nil;
}
#define NSClassFromString lookup
#include "QcomIos16LogStore.h"
int main(){@autoreleasepool {
 using namespace QcomUserLog;
 NSError *error=nil;id store;
 @autoreleasepool {store=openIos16SystemStore([FixtureStore class],&error);}
 assert(store&&!error&&lastSource);
 assert(object_getIvar(store,class_getInstanceVariable([FixtureStore class],"_source"))==lastSource);
 store=nil;assert(!lastSource); // The adapter's strong source ownership is balanced.
 unsigned before=prepares;
 assert(!openIos16SystemStore([WrongTypeStore class],&error)&&error&&prepares==before);
 assert(!openIos16SystemStore([MissingInitStore class],nullptr)&&prepares==before);
 assert(!openIos16SystemStore(Nil,&error)&&error&&prepares==before);
 assert(!openIos16SystemStore([FixtureStore class],&error,-1)&&prepares==before);
 factoryAvailable=false;
 assert(!openIos16SystemStore([FixtureStore class],&error)&&error&&prepares==before);
 factoryAvailable=true;mode=1;
 assert(!openIos16SystemStore([FixtureStore class],&error)&&error.code==13);
 assert([error.domain isEqualToString:@"native.logging.denial"]);
 mode=2;assert(!openIos16SystemStore([FixtureStore class],&error)&&error);
 mode=4;assert(!openIos16SystemStore([FixtureStore class],&error)&&error);
 mode=3;assert(!openIos16SystemStore([FixtureStore class],&error,0)&&error&&pending);
 pending([FixtureSource new],nil);pending=nil; // Completion after timeout must not touch caller stack.
 mode=0;initialState=1;assert(!openIos16SystemStore([FixtureStore class],&error)&&error);
 initialState=2;assert(!openIos16SystemStore([FixtureStore class],&error)&&error);
}}
'''
        with tempfile.TemporaryDirectory() as directory:
            d = Path(directory); (d/'test.mm').write_text(source)
            result = subprocess.run(['clang++','-std=c++17','-fobjc-arc','-Wall','-Wextra','-Werror',
                                     '-fsanitize=address,undefined','-framework','Foundation',
                                     '-I'+str(ROOT/'userland'),str(d/'test.mm'),'-o',str(d/'test')],capture_output=True)
            self.assertEqual(result.returncode,0,result.stderr.decode())
            subprocess.run([str(d/'test')],check=True,timeout=15)


if __name__ == '__main__':
    unittest.main()
