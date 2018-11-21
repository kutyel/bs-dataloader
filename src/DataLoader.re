type options = {
  batch: bool,
  maxBatchSize: int,
  cache: bool,
};

module type Impl = {
  type key;
  type value;
  type t('a);
  let batchLoadFun: array(key) => t(array(Belt.Result.t(value, exn)));
  let options: options;
  let all: array(t('a)) => unit;
  let resolve: 'a => t('a);
  let reject: 'b => t(unit);
  let then_: ('b => unit, t('a)) => unit;
  let make: (('a, t('a)) => unit, ('b, t('a)) => t(unit)) => t('a);
};

let firstNInQueueToArray = (queue, numberOfValues) => {
  let queueLength = Belt.MutableQueue.size(queue);
  let maxValues = queueLength > numberOfValues ? numberOfValues : queueLength;
  Belt.Array.makeBy(maxValues, _ => Belt.MutableQueue.popExn(queue));
};

module Make = (Impl: Impl) => {
  let shouldCache = Impl.options.cache;
  let shouldBatch = Impl.options.batch;
  let maxBatchSize = Impl.options.maxBatchSize;
  let batchLoadFun = Impl.batchLoadFun;
  let promiseCache: Hashtbl.t(Impl.key, Impl.t(Impl.value)) =
    Hashtbl.create(10);
  let queue = Belt.MutableQueue.make();
  let clear = key => Hashtbl.remove(promiseCache, key);
  let clearAll = () => Hashtbl.clear(promiseCache);
  let enqueuePostPromiseJob = fn => {
    let _ =
      Impl.resolve() /* TODO: replace `setTimeout` with something native */
      ->Impl.then_(
          _ => {
            let _ = Js.Global.setTimeout(fn, 1);
            Impl.resolve()->ignore;
          },
          _,
        );
    ();
  };
  let prime = (key, value) =>
    if (Hashtbl.mem(promiseCache, key)) {
      ();
    } else {
      Hashtbl.add(promiseCache, key, Impl.resolve(value));
    };
  let dispatchQueueBatch = queueSlice => {
    let keys = Belt.Array.map(queueSlice, ((key, _, _)) => key);
    batchLoadFun(keys)
    ->Impl.then_(
        values => {
          queueSlice
          ->Belt.Array.forEachWithIndex((index, (key, resolve, reject)) =>
              switch (values[index]) {
              | Belt.Result.Ok(value) => resolve(. value)
              | Belt.Result.Error(err) =>
                clear(key);
                reject(. err);
              }
            )
          ->ignore;
          Impl.resolve()->ignore;
        },
        _,
      )
    ->ignore;
    ();
  };
  let rec dispatchQueue = () =>
    if (Belt.MutableQueue.isEmpty(queue) == false) {
      dispatchQueueBatch(firstNInQueueToArray(queue, maxBatchSize));
      dispatchQueue();
    } else {
      ();
    };
  let addToQueue = item => Belt.MutableQueue.add(queue, item);
  let load = (key: Impl.key) =>
    if (shouldCache && Hashtbl.mem(promiseCache, key)) {
      Hashtbl.find(promiseCache, key);
    } else {
      let promise =
        Impl.make((resolve, reject) => {
          let _ = addToQueue((key, resolve, reject));
          if (Belt.MutableQueue.size(queue) == 1) {
            if (shouldBatch) {
              enqueuePostPromiseJob(dispatchQueue);
            } else {
              dispatchQueue();
            };
            ();
          } else {
            dispatchQueue();
          };
        });
      if (shouldCache) {
        Hashtbl.add(promiseCache, key, promise);
        promise;
      } else {
        promise;
      };
    };
  let loadMany = keys => Impl.all(Belt.Array.map(keys, load));
};